#!/bin/bash

# Add xhost command to .bashrc if not already present
if ! grep -Fxq "xhost +local:root" ~/.bashrc; then
    echo "xhost +local:root" >> ~/.bashrc
fi
