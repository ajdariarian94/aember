#!/bin/bash

set -e

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║           Aember Development Environment Setup               ║"
echo "║                 C++26 PID1 Init System                       ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

###############################################################################
# Check required tools
###############################################################################

echo "🔍 Checking required tools..."

MISSING_TOOLS=()

if ! command -v docker &>/dev/null; then
    MISSING_TOOLS+=("docker")
fi

if ! command -v git &>/dev/null; then
    MISSING_TOOLS+=("git")
fi

if ! command -v code &>/dev/null; then
    echo "  ⚠️  VS Code (code) not found in PATH — make sure it's installed"
    echo "     Download from: https://code.visualstudio.com/"
else
    echo "  ✅ VS Code found"
fi

# Check Dev Containers extension
if command -v code &>/dev/null; then
    if code --list-extensions 2>/dev/null | grep -q "ms-vscode-remote.remote-containers"; then
        echo "  ✅ Dev Containers extension installed"
    else
        echo "  ⚠️  Dev Containers extension not found — installing..."
        code --install-extension ms-vscode-remote.remote-containers
    fi
fi

if [ ${#MISSING_TOOLS[@]} -ne 0 ]; then
    echo ""
    echo "  ❌ Missing required tools: ${MISSING_TOOLS[*]}"
    echo "     Please install them and re-run this script."
    exit 1
fi

echo "  ✅ Docker found ($(docker --version | cut -d' ' -f3 | tr -d ','))"
echo "  ✅ Git found ($(git --version | cut -d' ' -f3))"
echo ""

###############################################################################
# .env secrets file — must be first so token is available for login
###############################################################################

echo "🔐 Setting up secrets file..."

ENV_FILE="$(dirname "$0")/../scripts/.env"
ENV_EXAMPLE="$(dirname "$0")/../scripts/.env.example"

if [ ! -f "$ENV_FILE" ]; then
    if [ -f "$ENV_EXAMPLE" ]; then
        cp "$ENV_EXAMPLE" "$ENV_FILE"
    else
        cat > "$ENV_FILE" << 'ENVEOF'
# Aember secrets — never commit this file
# Fill in your values and press ENTER to continue

export GITHUB_TOKEN=""
ENVEOF
    fi

    echo "  ✅ Created tools/scripts/.env"
    echo ""
    echo "  ⚠️  Please fill in your GITHUB_TOKEN:"
    echo "     nano tools/scripts/.env"
    echo ""
    echo "  💡 Token needs: read:packages scope"
    echo "     Generate at: https://github.com/settings/tokens"
    echo ""
    read -p "  Press ENTER when done to continue setup..."
    echo ""
fi

# Load secrets
source "$ENV_FILE"
echo "  ✅ Secrets loaded"
echo ""

###############################################################################
# GitHub Container Registry login
###############################################################################

echo "🐳 Setting up GitHub Container Registry access..."

if [ -n "$GITHUB_TOKEN" ]; then
    echo "  🔑 Logging into ghcr.io with GITHUB_TOKEN..."
    echo "$GITHUB_TOKEN" | docker login ghcr.io -u ajdariarian94 --password-stdin
    echo "  ✅ Logged into ghcr.io successfully"
else
    echo "  ⚠️  GITHUB_TOKEN is empty in .env — skipping ghcr.io login"
    echo "     Fill in tools/scripts/.env and re-run this script"
    echo "     The repo goes public before CppCon — no token needed after that"
fi
echo ""

###############################################################################
# AppArmor — required for Yocto inside Docker
###############################################################################

echo "🔒 Configuring AppArmor for Yocto builds inside Docker..."

SYSCTL_CONF="/etc/sysctl.d/99-yocto-docker-userns.conf"
SYSCTL_LINE="kernel.apparmor_restrict_unprivileged_userns = 0"

if ! sudo grep -Fxq "$SYSCTL_LINE" "$SYSCTL_CONF" 2>/dev/null; then
    echo "  Configuring AppArmor unprivileged user namespaces..."
    echo "$SYSCTL_LINE" | sudo tee "$SYSCTL_CONF" >/dev/null
fi

sudo sysctl -p "$SYSCTL_CONF" >/dev/null
echo "  ✅ AppArmor configured for Yocto"
echo ""

###############################################################################
# X11 access for QEMU display
###############################################################################

echo "🖥️  Configuring X11 access for QEMU windows..."

if ! grep -Fxq "xhost +local:root" ~/.bashrc; then
    echo "xhost +local:root" >> ~/.bashrc
    echo "  ✅ X11 access configured"
else
    echo "  ✅ X11 access already configured"
fi
echo ""

###############################################################################
# Docker GID — ensures dev user can access Docker socket inside container
###############################################################################

echo "🐋 Configuring Docker group access..."

if getent group docker >/dev/null 2>&1; then
    DOCKER_GID=$(getent group docker | cut -d: -f3)

    if ! grep -Fxq "export DOCKER_GID=${DOCKER_GID}" ~/.bashrc; then
        echo "export DOCKER_GID=${DOCKER_GID}" >> ~/.bashrc
        echo "  ✅ DOCKER_GID=${DOCKER_GID} added to ~/.bashrc"
    else
        echo "  ✅ DOCKER_GID already configured (${DOCKER_GID})"
    fi
else
    echo "  ⚠️  docker group not found — is Docker installed and running?"
fi
echo ""

###############################################################################
# Git submodules
###############################################################################

echo "📦 Initializing git submodules (vcpkg, aember-cli, yocto/poky)..."

if [ -f ".gitmodules" ]; then
    git submodule init
    git submodule update
    echo "  ✅ Submodules initialized"
else
    echo "  ⚠️  No .gitmodules found — are you in the aember root directory?"
fi
echo ""

###############################################################################
# Done
###############################################################################

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║                    Setup Complete! 🚀                        ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "  Next steps:"
echo "  1. Open VS Code in this directory: code ."
echo "  2. Click 'Reopen in Container' when prompted"
echo "  3. Wait for the container to start (~30s on first run)"
echo ""