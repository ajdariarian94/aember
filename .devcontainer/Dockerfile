# ---- Base image and argument setup ----
ARG BASE_IMAGE=ubuntu:noble
FROM ${BASE_IMAGE} AS base

ENV DEBIAN_FRONTEND=noninteractive

# Build arguments for matching host user
ARG UID=1000
ARG GID=1000
ARG USERNAME=dev

# Create group and user only if they don't exist already
# Create or reuse user with desired UID/GID
RUN \
    EXISTING_USER="$(getent passwd ${UID} | cut -d: -f1)" && \
    if [ -n "$EXISTING_USER" ] && [ "$EXISTING_USER" != "${USERNAME}" ]; then \
        echo "Renaming existing user $EXISTING_USER to ${USERNAME}"; \
        usermod -l "${USERNAME}" "$EXISTING_USER"; \
        usermod -d /home/${USERNAME} -m "${USERNAME}"; \
    elif [ -z "$EXISTING_USER" ]; then \
        echo "Creating new user ${USERNAME}"; \
        groupadd -f -g "${GID}" "${USERNAME}" || true; \
        useradd -m -u "${UID}" -g "${GID}" "${USERNAME}"; \
    fi

# ---- Install system dependencies ----
RUN apt update && apt install -y    \
    apt-utils                       \
    build-essential                 \
    clang                           \
    clang-tools                     \
    clang-tidy                      \
    git                             \
    ninja-build                     \
    curl                            \
    cppcheck                        \
    doxygen                         \
    gawk                            \
    gdb                             \
    gnupg                           \
    graphviz                        \
    gcc-aarch64-linux-gnu           \         
    gcc-arm-linux-gnueabihf         \
    g++-aarch64-linux-gnu           \
    g++-arm-linux-gnueabihf         \
    lcov                            \
    nsis                            \
    valgrind                        \
    liblxc-dev                      \
    libmbedtls-dev                  \
    zlib1g-dev                      \
    meson-1.5                       \
    libdbus-1-dev                   \
    docbook2x                       \
    autoconf                        \
    automake                        \
    autoconf-archive                \
    bison                           \
    flex                            \
    libtool                         \
    mono-complete                   \
    pkgconf                         \
    libaudit-dev                    \
    libcap-dev                      \   
    libsystemd-dev                  \
    apt-transport-https             \
    software-properties-common      \
    zip                             \
    unzip                           \
    tar                             \
    vim                             \
    wget                            \
    locales                         \
    fontconfig                      \
    bash-completion                 \   
    figlet                          \
    ruby                            \
    python3                         \
    python3-pip                     \
    python3.12-venv                 \
    sudo                            \
    cpio                            \
    qemu-system-x86                 \
    qemu-user-static                \
    qemu-utils                      \
    qemu-system                     \
    chrpath                         \
    diffstat                        \
    xterm                           \
    kitty                           \
    zstd                            \
    x11-utils                       \
    lsb-release &&                  \
    gem install lolcat

RUN apt update && apt install -y \
    libgl1-mesa-dri \
    libegl-mesa0 \
    libglx-mesa0 \
    libwayland-egl1-mesa \
    libwayland-client0 \
    libwayland-cursor0 \
    libglfw3

# Add Kitware APT repo for latest CMake
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | \
    gpg --dearmor - | \
    tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null && \
    apt-add-repository "deb https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" && \
    apt update && apt install -y cmake

# Set up locale
RUN locale-gen en_US.UTF-8 && \
    update-locale LANG=en_US.UTF-8
ENV LANG=en_US.UTF-8


# ---- Setup virtual environment ----
RUN python3 -m venv /venv
RUN /venv/bin/pip install --upgrade pip

# Copy Python dependencies and install
COPY ./requirements.txt /usr
RUN /venv/bin/pip install --no-cache-dir -r /usr/requirements.txt

# ---- Clone and build BusyBox ----
RUN git clone https://github.com/mirror/busybox.git /tmp/busybox-src

# ---- Build BusyBox for x86_64 (native) ----
RUN mkdir -p /tmp/busybox-x86 && cp -r /tmp/busybox-src/* /tmp/busybox-x86 && \
    cd /tmp/busybox-x86 && \
    make distclean && \
    make defconfig && \
    sed -i 's/^CONFIG_TC=.*/# CONFIG_TC is not set/' .config && \
    sed -i 's/^CONFIG_IFCONFIG=.*/# CONFIG_IFCONFIG is not set/' .config && \
    sed -i 's/^CONFIG_ROUTE=.*/# CONFIG_ROUTE is not set/' .config && \
    make -j$(nproc) && \
    mkdir -p /usr/local/bin/x64-linux && \
    cp busybox /usr/local/bin/x64-linux/busybox

# ---- Build BusyBox for ARM64 (aarch64) ----
RUN mkdir -p /tmp/busybox-aarch64 && cp -r /tmp/busybox-src/* /tmp/busybox-aarch64 && \
    cd /tmp/busybox-aarch64 && \
    make distclean && \
    make defconfig && \
    sed -i 's/^CONFIG_TC=.*/# CONFIG_TC is not set/; \
            s/^CONFIG_IFCONFIG=.*/# CONFIG_IFCONFIG is not set/; \
            s/^CONFIG_ROUTE=.*/# CONFIG_ROUTE is not set/; \
            s/^CONFIG_NSENTER=.*/# CONFIG_NSENTER is not set/; \
            s/^CONFIG_UNSHARE=.*/# CONFIG_UNSHARE is not set/; \
            s/^CONFIG_SHA1_HWACCEL=.*/# CONFIG_SHA1_HWACCEL is not set/; \
            s/^CONFIG_SHA256_HWACCEL=.*/# CONFIG_SHA256_HWACCEL is not set/' .config && \
    yes "" | make CROSS_COMPILE=aarch64-linux-gnu- oldconfig && \
    make CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) && \
    mkdir -p /usr/local/bin/aarch64-linux && \
    cp busybox /usr/local/bin/aarch64-linux/busybox

# ---- Build BusyBox for ARM32 (armhf) ----
RUN mkdir -p /tmp/busybox-armhf && cp -r /tmp/busybox-src/* /tmp/busybox-armhf && \
    cd /tmp/busybox-armhf && \
    make distclean && \
    make defconfig && \
    sed -i 's/^CONFIG_TC=.*/# CONFIG_TC is not set/; \
            s/^CONFIG_IFCONFIG=.*/# CONFIG_IFCONFIG is not set/; \
            s/^CONFIG_ROUTE=.*/# CONFIG_ROUTE is not set/; \
            s/^CONFIG_NSENTER=.*/# CONFIG_NSENTER is not set/; \
            s/^CONFIG_UNSHARE=.*/# CONFIG_UNSHARE is not set/; \
            s/^CONFIG_SHA1_HWACCEL=.*/# CONFIG_SHA1_HWACCEL is not set/; \
            s/^CONFIG_SHA256_HWACCEL=.*/# CONFIG_SHA256_HWACCEL is not set/' .config && \
    yes "" | make CROSS_COMPILE=arm-linux-gnueabihf- oldconfig && \
    make CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc) && \
    mkdir -p /usr/local/bin/arm-linux && \
    cp busybox /usr/local/bin/arm-linux/busybox

# Fix permissions (user was created earlier)
RUN chown -R ${UID}:${GID} /venv && chmod -R 755 /venv

# Clean up APT cache
RUN rm -rf /var/lib/apt/lists/*

# ---- Set default user and working directory ----
USER ${USERNAME}
WORKDIR /home/${USERNAME}
    