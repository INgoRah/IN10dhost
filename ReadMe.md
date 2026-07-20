# Introduction

## Features

## Limitations

# Building and running

## Requirements
apt install fuse3 libfuse3 libfuse3-dev nlohmann-json3-dev
if using gpio:
apt install libgpiod3 libgpiod-dev
sudo apt install libgtest-dev libgmock-dev

## native build
mkdir build-native
cmake -DUSE_GPIO=NO -DUSE_I2C=NO -S . -B build-native
cmake --build build-native

## Cross build
Requires
apt install fuse3:armhf libfuse3-4:armhf libfuse3-dev:armhf libgpiod3:armhf libgpiod-dev:armhf nlohmann-json3-dev
mkdir build-arm
cmake -DUSE_I2C=ON -DUSE_GPIO=NO \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -S . -B build-arm
cmake --build build-arm

## run as
sudo build-arm/bin/IN10dfsd -f -o allow_other /mnt/1wire

## Sbuild debian package

sudo sbuild-apt trixie-armhf apt-get install gpiod libgpiod-dev fuse3 libfuse3-dev

'''
sbuild --dist=trixie --arch=armhf --no-run-lintian --no-clean-source
'''

optional with -chroot=trixie-armhf

sbuild-shell trixie-armhf
apt-cache search libgpiod

## or copy
cmake --build build-arm && scp build-arm/bin/IN10dfsd root@192.168.178.79:/opt
cmake --build build-arm && scp build-arm/bin/IN10dfsd root@192.168.178.37:/data/home/ingo


## Publish to Docker
docker run -d \
  --name iobroker \
  --mount type=bind,source=/mnt/fuse,target=/data,bind-propagation=shared \
  my_image

docker run -p 8081:8081 --mount type=bind,source=/mnt/1wire,target=/mnt/1wire,bind-propagation=shared --name iobroker -v iobrokerdata:/opt/iobroker -h iobroker buanet/iobroker
