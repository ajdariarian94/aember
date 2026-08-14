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
# .env file — DOCKER_GID so the dev-container user can access the Docker
# socket on the host
###############################################################################

echo "🐋 Configuring Docker group access..."

ENV_FILE="$(dirname "$0")/../scripts/.env"
ENV_EXAMPLE="$(dirname "$0")/../scripts/.env.example"

if getent group docker >/dev/null 2>&1; then
    DOCKER_GID=$(getent group docker | cut -d: -f3)

    if [ ! -f "$ENV_FILE" ]; then
        if [ -f "$ENV_EXAMPLE" ]; then
            cp "$ENV_EXAMPLE" "$ENV_FILE"
        else
            touch "$ENV_FILE"
            echo "# Aember environment — never commit this file" > "$ENV_FILE"
        fi
        echo "  ✅ Created tools/scripts/.env"
    fi

    if grep -q "^export DOCKER_GID=" "$ENV_FILE" 2>/dev/null; then
        # Update existing entry in place
        sed -i "s/^export DOCKER_GID=.*/export DOCKER_GID=${DOCKER_GID}/" "$ENV_FILE"
        echo "  ✅ DOCKER_GID updated in tools/scripts/.env (${DOCKER_GID})"
    else
        echo "export DOCKER_GID=${DOCKER_GID}" >> "$ENV_FILE"
        echo "  ✅ DOCKER_GID=${DOCKER_GID} added to tools/scripts/.env"
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
