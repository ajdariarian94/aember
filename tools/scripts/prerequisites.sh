#!/bin/bash

# Add xhost command to .bashrc if not already present
if ! grep -Fxq "xhost +local:root" ~/.bashrc; then
    echo "xhost +local:root" >> ~/.bashrc
fi

# Initialize and update git submodules if not already done
if [ -f ".gitmodules" ]; then
    echo "Initializing and updating git submodules..."
    git submodule init
    git submodule update
else
    echo "No git submodules found."
fi
