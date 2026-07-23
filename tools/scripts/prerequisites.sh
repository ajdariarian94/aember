#!/bin/bash

set -e

###############################################################################
# Permanently disable AppArmor restriction for unprivileged user namespaces
# (Required for Yocto inside Docker)
###############################################################################

SYSCTL_CONF="/etc/sysctl.d/99-yocto-docker-userns.conf"
SYSCTL_LINE="kernel.apparmor_restrict_unprivileged_userns = 0"

# Create sysctl config if not already present
if ! sudo grep -Fxq "$SYSCTL_LINE" "$SYSCTL_CONF" 2>/dev/null; then
    echo "Configuring permanent AppArmor userns setting..."
    echo "$SYSCTL_LINE" | sudo tee "$SYSCTL_CONF" >/dev/null
fi

# Apply immediately
echo "Applying AppArmor userns setting..."
sudo sysctl -p "$SYSCTL_CONF"

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

###############################################################################
# .env secrets file
###############################################################################

ENV_FILE="$(dirname "$0")/../scripts/.env"
ENV_EXAMPLE="$(dirname "$0")/../scripts/.env.example"

if [ ! -f "$ENV_FILE" ]; then
    if [ -f "$ENV_EXAMPLE" ]; then
        cp "$ENV_EXAMPLE" "$ENV_FILE"
        echo "Created .env from .env.example — fill in your GITHUB_TOKEN"
    else
        cat > "$ENV_FILE" << 'ENVEOF'
# Aember secrets — never commit this file
# Fill in your values and restart the dev container

export GITHUB_TOKEN=""
ENVEOF
        echo "Created empty .env — fill in your GITHUB_TOKEN"
    fi
else
    echo ".env already exists — skipping"
fi