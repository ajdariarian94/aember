############################################################
# Base image for Linux (common dependencies)
############################################################

ARG BASE_IMAGE=ubuntu:24.04

FROM ${BASE_IMAGE} AS base

ENV DEBIAN_FRONTEND=noninteractive

ARG UID=1000
ARG GID=1000
ARG USERNAME=dev

RUN getent group ${GID} || groupadd -g ${GID} ${USERNAME}
RUN id -u ${UID} &>/dev/null || useradd -u ${UID} -g ${GID} ${USERNAME}

# Install base dependencies
RUN apt update && apt install -y \
    build-essential \
    clang \
    clang-tools \
    clang-tidy \
    git \
    ninja-build

# Install the development additional dependencies
RUN apt update && apt install -y \
    curl \
    cppcheck \
    doxygen \
    gawk \
    gdb \
    graphviz \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    lcov \
    nsis \
    valgrind \ 
    liblxc-dev \ 
    libmbedtls-dev \ 
    zlib1g-dev      
   
RUN dpkg --add-architecture i386

# Install additional tools
RUN apt update && apt install -y \
    zip \
    unzip \
    tar \
    vim \
    wget \
    locales \
    fontconfig \
    wine64 \
    wine32 
    

RUN apt update && apt install -y \ 
    meson-1.5 \
    libdbus-1-dev \
    docbook2x

# Tools used from Vcpkg
RUN apt update && apt install -y \
    autoconf \
    automake \
    autoconf-archive \
    bison \
    flex  \
    libtool \
    mono-complete \
    pkgconf

# Install Linux only dependencies
RUN apt update && apt install -y \
    libaudit-dev \
    libcap-dev \
    libsystemd-dev

# Install Powershell
RUN apt update && apt install -y \
    apt-transport-https \
    software-properties-common

# Install Python and PyInstaller
#RUN apt update && apt install -y \
#    python3 \
#    python3-pip && \
#    pip3 install pyinstaller

# Remove apt list
RUN rm -rf /var/lib/apt/lists/*

# Setup default font and locale
RUN locale-gen en_US.UTF-8 && \
    update-locale LANG=en_US.UTF-8
ENV LANG=en_US.UTF-8


# Install newest version of cmake
RUN apt update && apt install -y software-properties-common lsb-release && apt clean all
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
RUN apt-add-repository "deb https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main"
RUN apt update && apt install -y cmake


USER ${USERNAME}
WORKDIR /home/${USERNAME}
