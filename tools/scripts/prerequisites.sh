#!/bin/bash

set -e

###############################################################################
# X11 access for containers
###############################################################################
if ! grep -Fxq "xhost +local:root" ~/.bashrc; then
    echo "xhost +local:root" >> ~/.bashrc
fi

###############################################################################
# Export Docker GID (host)
###############################################################################
if getent group docker >/dev/null 2>&1; then
    DOCKER_GID=$(getent group docker | cut -d: -f3)

    # Add export only once
    if ! grep -Fxq "export DOCKER_GID=${DOCKER_GID}" ~/.bashrc; then
        echo "export DOCKER_GID=${DOCKER_GID}" >> ~/.bashrc
        echo "Added DOCKER_GID=${DOCKER_GID} to ~/.bashrc"
    fi
else
    echo "[warn] docker group not found on host"
fi

###############################################################################
# Git submodules
###############################################################################
if [ -f ".gitmodules" ]; then
    echo "Initializing and updating git submodules..."
    git submodule init
    git submodule update
else
    echo "No git submodules found."
fi
