rm -rf build
mkdir build
cd build
cmake -DTSMUXER_STATIC_BUILD=ON ../
make
cp tsMuxer/tsmuxer ../bin/tsMuxeR
cd ..
rm -rf build
mkdir ./bin/lnx
mv ./bin/tsMuxeR ./bin/lnx/tsMuxeR

zip -r ./bin/lnx.zip ./bin/lnx
ls ./bin/lnx.zip
