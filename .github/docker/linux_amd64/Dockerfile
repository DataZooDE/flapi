FROM ubuntu:24.04

# Setup the basic necessities
RUN apt-get update -y -qq
RUN apt-get install -y -qq software-properties-common
RUN apt-get install -y -qq --fix-missing \
    git \
    ninja-build \
    cmake \
    make \
    gcc-multilib \
    g++-multilib \
    libssl-dev \
    wget \
    zip \
    unixodbc-dev \
    libc6-dev-i386 \
    lib32readline6-dev \
    libssl-dev \
    libcurl4-gnutls-dev \
    libexpat1-dev \
    gettext \
    unzip \
    build-essential \
    checkinstall \
    libffi-dev \
    curl \
    libz-dev \
    openssh-client \
    pkg-config \
    autoconf

# Install cmake 3.21
RUN mkdir /cmake_3_21 && \
    cd /cmake_3_21 && \
    wget https://github.com/Kitware/CMake/releases/download/v3.21.3/cmake-3.21.3-linux-x86_64.sh && \
    chmod +x cmake-3.21.3-linux-x86_64.sh && \
    ./cmake-3.21.3-linux-x86_64.sh --skip-license --prefix=/usr/local && \
    cmake --version

# Common environment variables
ENV GEN=ninja

# Specify where we expect the extension to be mounted and use that as working dir
VOLUME /build_dir
WORKDIR /build_dir

# Mount for ccache to allow restoring ccache in GH actions
VOLUME /ccache_dir
ENV CCACHE_DIR=/ccache_dir
ENV CCACHE_COMPRESS=TRUE
ENV CCACHE_COMPRESSLEVEL=6
ENV CCACHE_MAXSIZE=400M

# Setup VCPKG n a mounted volume
ARG vcpkg_url=https://github.com/microsoft/vcpkg.git
ARG vcpkg_tag=2024.11.16
RUN mkdir /vcpkg && \
    cd /vcpkg && \
    git init && \
    git remote add origin $vcpkg_url && \
    git fetch origin $vcpkg_commit && \
    git checkout $vcpkg_tag && \
    ./bootstrap-vcpkg.sh
ENV VCPKG_ROOT=/vcpkg
ENV VCPKG_TOOLCHAIN_PATH=/vcpkg/scripts/buildsystems/vcpkg.cmake