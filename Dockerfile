# ---- Base image and argument setup ----
ARG BASE_IMAGE=ubuntu:noble
FROM ${BASE_IMAGE} AS base

ENV DEBIAN_FRONTEND=noninteractive

# Build arguments for matching host user
ARG UID=1000
ARG GID=1000
ARG USERNAME

# Create group and user only if they don't exist already
RUN if ! getent group "${GID}" >/dev/null 2>&1; then \
        groupadd -g "${GID}" "${USERNAME}"; \
    else \
        echo "Group with GID ${GID} already exists, skipping groupadd"; \
    fi && \
    if ! id -u "${UID}" >/dev/null 2>&1; then \
        useradd -m -u "${UID}" -g "${GID}" "${USERNAME}"; \
    else \
        echo "User with UID ${UID} already exists, skipping useradd"; \
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
    g++-aarch64-linux-gnu           \
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
    qemu-utils                      \
    qemu-system                     \
    chrpath                         \
    diffstat                        \
    xterm                           \
    kitty                           \
    zstd                            \
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

# Fix permissions (user was created earlier)
RUN chown -R ${UID}:${GID} /venv && chmod -R 755 /venv

# Clean up APT cache
RUN rm -rf /var/lib/apt/lists/*

# ---- Set default user and working directory ----
USER ${USERNAME}
WORKDIR /home/${USERNAME}
    