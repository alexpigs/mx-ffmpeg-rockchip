#! /bin/bash

rm -rf ./deb/usr
rm -rf ./mxffmpeg.tar

for i in $(ldd ./ffmpeg | awk '{print $3}') 
do
    # cp /lib/aarch64-linux-gnu/libk5crypto.so.3 to ./build/src/lib/aarch64-linux-gnu/
    # cp /lib/* to ./build/src/lib/

    # remove first char of i
    dst=${i:1}
    # get dst dir
    x=$(dirname $dst)
    # create dir
    mkdir -p ./deb/usr/$x
    cp $i ./deb/usr/$x
done

# copy v2c
mkdir ./deb/usr/bin 
cp ./ffmeg ./deb/usr/bin/

#tar
cd ./deb/usr
tar -cvf ../../mxffmpeg.tar .
cd ../../