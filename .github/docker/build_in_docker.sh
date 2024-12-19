#! /bin/bash

BUILD_ARCH="arm64"
BUILD_IMAGE="flapi_build_${BUILD_ARCH}"

# Build the docker image
docker build \
    -t ${BUILD_IMAGE} \
    -f /`pwd`/.github/docker/linux_${BUILD_ARCH}/Dockerfile .

# Run the build in docker
docker run \
    -it --rm \
    -e FLAPI_CROSS_COMPILE="${BUILD_ARCH}" \
    -v `pwd`:/build_dir \
    ${BUILD_IMAGE} \
    bash -c 'make clean && make release'
