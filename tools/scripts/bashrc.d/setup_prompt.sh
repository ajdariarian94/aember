#!/bin/bash
# Prompt and aember symlink setup

BASHRC="$HOME/.bashrc"
WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)/aember_ws"

USER_COLOR='\033[32m'
DOCKER_COLOR='\033[34m'
PATH_COLOR='\033[35m'
COMMAND_COLOR='\033[38;5;214m'
BOLD='\033[1m'
RESET_COLOR='\033[0m'

# --- Update .bashrc ---
cat <<EOF >> "$BASHRC"

# BEGIN AEMBER PROMPT
# Add /venv/bin to PATH if missing
if [[ ":\$PATH:" != *":/venv/bin:"* ]]; then
    export PATH="/venv/bin:\$PATH"
fi

PS1='${BOLD}\[${USER_COLOR}\]\u\[${RESET_COLOR}\]@${BOLD}\[${DOCKER_COLOR}\]aember-dev\[${RESET_COLOR}\]:${BOLD}\[${PATH_COLOR}\]\w\[${RESET_COLOR}\]${COMMAND_COLOR}\$ ${RESET_COLOR}'
# END AEMBER PROMPT
EOF

# --- Apply immediately ---
if [[ ":$PATH:" != *":/venv/bin:"* ]]; then
    export PATH="/venv/bin:$PATH"
fi

PS1='${BOLD}\[${USER_COLOR}\]\u\[${RESET_COLOR}\]@${BOLD}\[${DOCKER_COLOR}\]aember-dev\[${RESET_COLOR}\]:${BOLD}\[${PATH_COLOR}\]\w\[${RESET_COLOR}\]${COMMAND_COLOR}\$ ${RESET_COLOR}'

# --- Symlink aember in /venv/bin ---
mkdir -p /venv/bin
ln -sf "$WORKSPACE_DIR/python/run.py" /venv/bin/aember
