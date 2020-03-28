#include "hevcStreamReader.h"

#include <fs/systemlog.h>

#include "hevc.h"
#include "nalUnits.h"
#include "tsMuxer.h"
#include "tsPacket.h"
#include "vodCoreException.h"

using namespace std;

static const int MAX_SLICE_HEADER = 64;
static const int HEVC_DESCRIPTOR_TAG = 0x38;

HEVCStreamReader::HEVCStreamReader()
    : MPEGStreamReader(),
      m_vps(0),
      m_sps(0),
      m_pps(0),
      m_hdr(0),
      m_firstFrame(true),
      m_frameNum(0),
      m_fullPicOrder(0),
      m_frameDepth(1),

      m_picOrderMsb(0),
      m_prevPicOrder(0),
      m_picOrderBase(0),
      m_lastIFrame(false),
      m_firstFileFrame(false),
      m_vpsCounter(0),
      m_vpsSizeDiff(0)
{
}

HEVCStreamReader::~HEVCStreamReader()
{
    delete m_vps;
    delete m_sps;
    delete m_pps;
    delete m_hdr;
}

CheckStreamRez HEVCStreamReader::checkStream(uint8_t* buffer, int len)
{
    CheckStreamRez rez;

    uint8_t* end = buffer + len;
    for (uint8_t* nal = NALUnit::findNextNAL(buffer, end); nal < end - 4; nal = NALUnit::findNextNAL(nal, end))
    {
        if (*nal & 0x80)
            return rez;  // invalid nal
        int nalType = (*nal >> 1) & 0x3f;
        uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, end, true);

        switch (nalType)
        {
        case NAL_VPS:
        {
            if (!m_vps)
                m_vps = new HevcVpsUnit();
            m_vps->decodeBuffer(nal, nextNal);
            if (m_vps->deserialize())
                return rez;
            m_spsPpsFound = true;
            if (m_vps->num_units_in_tick)
                updateFPS(m_vps, nal, nextNal, 0);
            break;
        }
        case NAL_SPS:
        {
            if (!m_sps)
                m_sps = new HevcSpsUnit();
            m_sps->decodeBuffer(nal, nextNal);
            if (m_sps->deserialize() != 0)
                return rez;
            m_spsPpsFound = true;
            updateFPS(m_sps, nal, nextNal, 0);
            break;
        }
        case NAL_PPS:
        {
            if (!m_pps)
                m_pps = new HevcPpsUnit();
            m_pps->decodeBuffer(nal, nextNal);
            if (m_pps->deserialize() != 0)
                return rez;
            break;
        }
        case NAL_SEI_PREFIX:
        {
            if (!m_hdr)
                m_hdr = new HevcHdrUnit();
            m_hdr->decodeBuffer(nal, nextNal);
            if (m_hdr->deserialize() != 0)
                return rez;
            break;
        }
        case NAL_DVRPU:
        case NAL_DVEL:
        {
            if (!m_hdr)
                m_hdr = new HevcHdrUnit();
            if (nal[1] == 1)
            {
                if (nalType == NAL_DVEL)
                    m_hdr->isDVEL = true;
                else
                    m_hdr->isDVRPU = true;
                V3_flags |= DV;
            }
            break;
        }
        }
    }

    if (m_vps && m_sps && m_pps && m_sps->vps_id == m_vps->vps_id && m_pps->sps_id == m_sps->sps_id)
    {
        int cp = m_sps->colour_primaries;
        int tc = m_sps->transfer_characteristics;
        int mc = m_sps->matrix_coeffs;
        int cslt = m_sps->chroma_sample_loc_type_top_field;

        if (!m_hdr)
            m_hdr = new HevcHdrUnit();

        // cf. "DolbyVisionProfilesLevels_v1_3_2_2019_09_16.pdf"
        if (cp == 9 && tc == 16 && mc == 9)  // BT.2100 colorspace
        {
            m_hdr->isHDR10 = true;
            if (cslt == 2)
                m_hdr->DVCompatibility = 6;
            else if (cslt == 0)
                m_hdr->DVCompatibility = 1;
            V3_flags |= HDR10;
        }
        else if (cp == 9 && tc == 18 && mc == 9 && cslt == 2)  // ARIB HLG
            m_hdr->DVCompatibility = 4;
        else if (cp == 9 && tc == 14 && mc == 9 && cslt == 0)  // DVB HLG
            m_hdr->DVCompatibility = 4;
        else if (cp == 1 && tc == 1 && mc == 1 && cslt == 0)  // SDR
            m_hdr->DVCompatibility = 2;
        else if (cp == 2 && tc == 2 && mc == 2 && cslt == 0)  // Undefined
        {
            if (m_hdr->isDVEL)
                m_hdr->DVCompatibility = 2;
            else
                m_hdr->DVCompatibility = 0;
        }

        rez.codecInfo = hevcCodecInfo;
        rez.streamDescr = m_sps->getDescription();
        size_t frSpsPos = rez.streamDescr.find("Frame rate: not found");
        if (frSpsPos != string::npos)
            rez.streamDescr = rez.streamDescr.substr(0, frSpsPos) + string(" ") + m_vps->getDescription();
    }

    return rez;
}

int HEVCStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode)
{
    if (m_firstFrame)
        CheckStreamRez rez = checkStream(m_buffer, m_bufEnd - m_buffer);

    /* non-HDMV descriptor, for future use
    if (!blurayMode && (V3_flags & DV))
        for (uint8_t* nal = NALUnit::findNextNAL(m_buffer, m_bufEnd); nal < m_bufEnd - 4;
             nal = NALUnit::findNextNAL(nal, m_bufEnd))
        {
            uint8_t nalType = (*nal >> 1) & 0x3f;
            uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, m_bufEnd, true);

            if (nalType == NAL_SPS)
            {
                uint8_t tmpBuffer[512];
                int toDecode = FFMIN(sizeof(tmpBuffer) - 8, nextNal - nal);
                int decodedLen = NALUnit::decodeNAL(nal, nal + toDecode, tmpBuffer, sizeof(tmpBuffer));

                int lenDoviDesc = 0;
                if (m_hdr->isDVEL || m_hdr->isDVRPU)
                {
                    lenDoviDesc = setDoViDescriptor(dstBuff);
                    dstBuff += lenDoviDesc;
                }

                *dstBuff++ = HEVC_DESCRIPTOR_TAG;
                *dstBuff++ = 13;  // descriptor length
                memcpy(dstBuff, tmpBuffer + 3, 12);
                dstBuff += 12;
                // temporal_layer_subset, HEVC_still_present, HEVC_24hr_picture_present, HDR_WCG unspecified
                *dstBuff = 0x0f;

                if (!m_sps->sub_pic_hrd_params_present_flag)
                    *dstBuff |= 0x10;
                dstBuff++;

                // HEVC_timing_and_HRD_descriptor
                memcpy(dstBuff, "\x3f\x0f\x03\x7f\x7f", 5);
                dstBuff += 5;

                uint32_t N = 1001 * getFPS();
                uint32_t K = 27000000;
                uint32_t num_units_in_tick = 1001;
                if (N % 1000)
                {
                    N = 1000 * getFPS();
                    num_units_in_tick = 1000;
                }
                N = my_htonl(N);
                K = my_htonl(K);
                num_units_in_tick = my_htonl(num_units_in_tick);
                memcpy(dstBuff, &N, 4);
                dstBuff += 4;
                memcpy(dstBuff, &K, 4);
                dstBuff += 4;
                memcpy(dstBuff, &num_units_in_tick, 4);
                dstBuff += 4;

                return 32 + lenDoviDesc;
            }
        } */

    // 'HDMV' registration descriptor
    *dstBuff++ = 0x05;
    *dstBuff++ = 8;
    memcpy(dstBuff, "HDMV\xff\x24", 6);
    dstBuff += 6;

    int video_format, frame_rate_index, aspect_ratio_index;
    M2TSStreamInfo::blurayStreamParams(getFPS(), getInterlaced(), getStreamWidth(), getStreamHeight(), getStreamAR(),
                                       &video_format, &frame_rate_index, &aspect_ratio_index);

    *dstBuff++ = (video_format << 4) + frame_rate_index;
    *dstBuff++ = (aspect_ratio_index << 4) + 0xf;

    int lenDoviDesc = 0;
    if (!blurayMode && (m_hdr->isDVEL || m_hdr->isDVRPU))
    {
        lenDoviDesc = setDoViDescriptor(dstBuff);
        dstBuff += lenDoviDesc;
    }

    return 10 + lenDoviDesc;
}

int HEVCStreamReader::setDoViDescriptor(uint8_t* dstBuff)
{
    int isDVBL = !(V3_flags & NON_DV_TRACK);
    if (!isDVBL)
        m_hdr->isDVEL = true;

    int width = getStreamWidth();
    if (!isDVBL && V3_flags & FOUR_K)
        width *= 2;

    int pixelRate = width * getStreamHeight() * getFPS();

    int level = 0;
    if (width <= 1280 && pixelRate <= 22118400)
        level = 1;
    else if (width <= 1280 && pixelRate <= 27648000)
        level = 2;
    else if (width <= 1920 && pixelRate <= 49766400)
        level = 3;
    else if (width <= 2560 && pixelRate <= 62208000)
        level = 4;
    else if (width <= 3840 && pixelRate <= 124416000)
        level = 5;
    else if (width <= 3840 && pixelRate <= 199065600)
        level = 6;
    else if (width <= 3840 && pixelRate <= 248832000)
        level = 7;
    else if (width <= 3840 && pixelRate <= 398131200)
        level = 8;
    else if (width <= 3840 && pixelRate <= 497664000)
        level = 9;
    else if (width <= 3840 && pixelRate <= 995328000)
        level = 10;
    else if (width <= 7680 && pixelRate <= 995328000)
        level = 11;
    else if (width <= 7680 && pixelRate <= 1990656000)
        level = 12;
    else if (width <= 7680 && pixelRate <= 3981312000)
        level = 13;

    BitStreamWriter bitWriter;
    bitWriter.setBuffer(dstBuff, dstBuff + 128);

    // 'DOVI' registration descriptor
    bitWriter.putBits(8, 5);
    bitWriter.putBits(8, 4);
    bitWriter.putBits(32, 0x444f5649);

    bitWriter.putBits(8, 0xb0);            // DoVi descriptor tag
    bitWriter.putBits(8, isDVBL ? 5 : 7);  // Length
    bitWriter.putBits(8, 1);               // dv version major
    bitWriter.putBits(8, 0);               // dv version minor
    // DV profile
    if (m_hdr->isDVEL)
        bitWriter.putBits(7, isDVBL ? 4 : 7);
    else
        bitWriter.putBits(
            7, (m_hdr->DVCompatibility == 1 || m_hdr->DVCompatibility == 2 || m_hdr->DVCompatibility == 4) ? 8 : 5);
    bitWriter.putBits(6, level);           // dv level
    bitWriter.putBits(1, m_hdr->isDVRPU);  // rpu_present_flag
    bitWriter.putBits(1, m_hdr->isDVEL);   // el_present_flag
    bitWriter.putBits(1, isDVBL);          // bl_present_flag
    if (!isDVBL)
    {
        bitWriter.putBits(13, 0x1011);  // dependency_pid
        bitWriter.putBits(3, 7);        // reserved
    }
    bitWriter.putBits(4, m_hdr->DVCompatibility);  // dv_bl_signal_compatibility_id
    bitWriter.putBits(4, 15);                      // reserved

    bitWriter.flushBits();
    return 8 + (isDVBL ? 5 : 7);
}

void HEVCStreamReader::updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int)
{
    int oldNalSize = nextNal - buff;
    m_vpsSizeDiff = 0;
    HevcVpsUnit* vps = (HevcVpsUnit*)nalUnit;
    vps->setFPS(m_fps);
    uint8_t* tmpBuffer = new uint8_t[vps->nalBufferLen() + 16];
    long newSpsLen = vps->serializeBuffer(tmpBuffer, tmpBuffer + vps->nalBufferLen() + 16);
    if (newSpsLen == -1)
        THROW(ERR_COMMON, "Not enough buffer");

    if (newSpsLen != oldNalSize)
    {
        m_vpsSizeDiff = newSpsLen - oldNalSize;
        if (m_bufEnd + m_vpsSizeDiff > m_tmpBuffer + TMP_BUFFER_SIZE)
            THROW(ERR_COMMON, "Not enough buffer");
        memmove(nextNal + m_vpsSizeDiff, nextNal, m_bufEnd - nextNal);
        m_bufEnd += m_vpsSizeDiff;
    }
    memcpy(buff, tmpBuffer, newSpsLen);

    delete[] tmpBuffer;
}

int HEVCStreamReader::getStreamWidth() const { return m_sps ? m_sps->pic_width_in_luma_samples : 0; }

int HEVCStreamReader::getStreamHeight() const { return m_sps ? m_sps->pic_height_in_luma_samples : 0; }

int HEVCStreamReader::getStreamHDR() const
{
    return (m_hdr->isDVRPU || m_hdr->isDVEL) ? 4 : (m_hdr->isHDR10plus ? 16 : (m_hdr->isHDR10 ? 2 : 1));
}

double HEVCStreamReader::getStreamFPS(void* curNalUnit)
{
    double fps = 0;
    if (m_vps)
        fps = m_vps->getFPS();
    if (fps == 0 && m_sps)
        fps = m_sps->getFPS();
    return fps;
}

bool HEVCStreamReader::isSlice(int nalType) const
{
    if (!m_sps || !m_vps || !m_pps)
        return false;
    return (nalType >= NAL_TRAIL_N && nalType <= NAL_RASL_R) ||
           (nalType >= NAL_BLA_W_LP && nalType <= NAL_RSV_IRAP_VCL23);
}

bool HEVCStreamReader::isSuffix(int nalType) const
{
    if (!m_sps || !m_vps || !m_pps)
        return false;
    return (nalType == NAL_FD_NUT || nalType == NAL_SEI_SUFFIX || nalType == NAL_RSV_NVCL45 ||
            (nalType >= NAL_RSV_NVCL45 && nalType <= NAL_RSV_NVCL47) ||
            (nalType >= NAL_UNSPEC56 && nalType <= NAL_DVEL));
}

void HEVCStreamReader::incTimings()
{
    if (m_totalFrameNum++ > 0)
        m_curDts += m_pcrIncPerFrame;
    int delta = m_frameNum - m_fullPicOrder;
    m_curPts = m_curDts - delta * m_pcrIncPerFrame;
    m_frameNum++;
    m_firstFrame = false;

    if (delta > m_frameDepth)
    {
        m_frameDepth = FFMIN(4, delta);
        LTRACE(LT_INFO, 2,
               "B-pyramid level " << m_frameDepth - 1 << " detected. Shift DTS to " << m_frameDepth << " frames");
    }
}

int HEVCStreamReader::toFullPicOrder(HevcSliceHeader* slice, int pic_bits)
{
    if (slice->isIDR())
    {
        m_picOrderBase = m_frameNum;
        m_picOrderMsb = 0;
        m_prevPicOrder = 0;
    }
    else
    {
        int range = 1 << pic_bits;

        if (slice->pic_order_cnt_lsb < m_prevPicOrder && m_prevPicOrder - slice->pic_order_cnt_lsb >= range / 2)
            m_picOrderMsb += range;
        else if (slice->pic_order_cnt_lsb > m_prevPicOrder && slice->pic_order_cnt_lsb - m_prevPicOrder >= range / 2)
            m_picOrderMsb -= range;

        m_prevPicOrder = slice->pic_order_cnt_lsb;
    }

    return slice->pic_order_cnt_lsb + m_picOrderMsb + m_picOrderBase;
}

void HEVCStreamReader::storeBuffer(MemoryBlock& dst, const uint8_t* data, const uint8_t* dataEnd)
{
    dataEnd--;
    while (dataEnd > data && dataEnd[-1] == 0) dataEnd--;
    if (dataEnd > data)
    {
        dst.resize(dataEnd - data);
        memcpy(dst.data(), data, dataEnd - data);
    }
}

int HEVCStreamReader::intDecodeNAL(uint8_t* buff)
{
    int rez = 0;
    bool sliceFound = false;
    m_spsPpsFound = false;
    m_lastIFrame = false;

    uint8_t* prevPos = 0;
    uint8_t* curPos = buff;
    uint8_t* nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);
    uint8_t* nextNalWithStartCode;
    long oldSpsLen = 0;

    if (!m_eof && nextNal == m_bufEnd)
        return NOT_ENOUGH_BUFFER;

    while (curPos < m_bufEnd)
    {
        int nalType = (*curPos >> 1) & 0x3f;
        if (isSlice(nalType))
        {
            if (curPos[2] & 0x80)  // slice.first_slice
            {
                if (sliceFound)
                {  // first slice of next frame: case where there is no non-VCL NAL between the two frames
                    m_lastDecodedPos = prevPos;  // next frame started
                    incTimings();
                    return 0;
                }
                else
                {  // first slice of current frame
                    HevcSliceHeader slice;
                    slice.decodeBuffer(curPos, FFMIN(curPos + MAX_SLICE_HEADER, nextNal));
                    rez = slice.deserialize(m_sps, m_pps);
                    if (rez)
                        return rez;  // not enough buffer or error
                    // if (slice.slice_type == HEVC_IFRAME_SLICE)
                    if (nalType >= NAL_BLA_W_LP)
                        m_lastIFrame = true;
                    m_fullPicOrder = toFullPicOrder(&slice, m_sps->log2_max_pic_order_cnt_lsb);
                }
            }
            sliceFound = true;
        }
        else if (!isSuffix(nalType))
        {  // first non-VCL prefix NAL (AUD, SEI...) following current frame
            if (sliceFound)
            {
                incTimings();
                m_lastDecodedPos = prevPos;  // next frame started
                return 0;
            }

            nextNalWithStartCode = nextNal[-4] == 0 ? nextNal - 4 : nextNal - 3;

            switch (nalType)
            {
            case NAL_VPS:
                if (!m_vps)
                    m_vps = new HevcVpsUnit();
                m_vps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_vps->deserialize();
                if (rez)
                    return rez;
                m_spsPpsFound = true;
                m_vpsCounter++;
                m_vpsSizeDiff = 0;
                if (m_vps->num_units_in_tick)
                    updateFPS(m_vps, curPos, nextNalWithStartCode, 0);
                nextNal += m_vpsSizeDiff;
                storeBuffer(m_vpsBuffer, curPos, nextNalWithStartCode);
                break;
            case NAL_SPS:
                if (!m_sps)
                    m_sps = new HevcSpsUnit();
                m_sps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_sps->deserialize();
                if (rez)
                    return rez;
                m_spsPpsFound = true;
                updateFPS(m_sps, curPos, nextNalWithStartCode, 0);
                storeBuffer(m_spsBuffer, curPos, nextNalWithStartCode);
                break;
            case NAL_PPS:
                if (!m_pps)
                    m_pps = new HevcPpsUnit();
                m_pps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_pps->deserialize();
                if (rez)
                    return rez;
                m_spsPpsFound = true;
                storeBuffer(m_ppsBuffer, curPos, nextNalWithStartCode);
                break;
            case NAL_SEI_PREFIX:
                if (!m_hdr)
                    m_hdr = new HevcHdrUnit();
                m_hdr->decodeBuffer(curPos, nextNal);
                if (m_hdr->deserialize() != 0)
                    return rez;
                break;
            }
        }
        prevPos = curPos;
        curPos = nextNal;
        nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);

        if (!m_eof && nextNal == m_bufEnd)
            return NOT_ENOUGH_BUFFER;
    }
    if (m_eof)
    {
        m_lastDecodedPos = m_bufEnd;
        return 0;
    }
    else
        return NEED_MORE_DATA;
}

uint8_t* HEVCStreamReader::writeNalPrefix(uint8_t* curPos)
{
    if (!m_shortStartCodes)
        *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 1;
    return curPos;
}

uint8_t* HEVCStreamReader::writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, uint8_t* dstEnd)
{
    if (srcData.isEmpty())
        return dstBuffer;
    int bytesLeft = dstEnd - dstBuffer;
    int requiredBytes = srcData.size() + 3 + (m_shortStartCodes ? 0 : 1);
    if (bytesLeft < requiredBytes)
        return dstBuffer;

    dstBuffer = writeNalPrefix(dstBuffer);
    memcpy(dstBuffer, srcData.data(), srcData.size());
    dstBuffer += srcData.size();
    return dstBuffer;
}

int HEVCStreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                        PriorityDataInfo* priorityData)
{
    uint8_t* curPos = dstBuffer;

    if (avPacket.size > 4 && avPacket.size < dstEnd - dstBuffer)
    {
        int offset = avPacket.data[2] == 1 ? 3 : 4;
        uint8_t nalType = (avPacket.data[offset] >> 1) & 0x3f;
        if (nalType == NAL_AUD)
        {
            // place delimiter at first place
            memcpy(curPos, avPacket.data, avPacket.size);
            curPos += avPacket.size;
            avPacket.size = 0;
            avPacket.data = 0;
        }
    }

    bool needInsSpsPps = m_firstFileFrame && !(avPacket.flags & AVPacket::IS_SPS_PPS_IN_GOP);
    if (needInsSpsPps)
    {
        avPacket.flags |= AVPacket::IS_SPS_PPS_IN_GOP;

        curPos = writeBuffer(m_vpsBuffer, curPos, dstEnd);
        curPos = writeBuffer(m_spsBuffer, curPos, dstEnd);
        curPos = writeBuffer(m_ppsBuffer, curPos, dstEnd);
    }

    m_firstFileFrame = false;
    return curPos - dstBuffer;
}
