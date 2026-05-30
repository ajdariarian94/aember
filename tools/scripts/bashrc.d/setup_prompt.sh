#!/bin/bash
# Prompt and aember symlink setup

BASHRC="$HOME/.bashrc"
WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)/aember_ws/aember"

# ANSI colors (actual escape codes)
USER_COLOR='\033[32m'
DOCKER_COLOR='\033[34m'
PATH_COLOR='\033[35m'
COMMAND_COLOR='\033[38;5;214m'
BOLD='\033[1m'
RESET_COLOR='\033[0m'

# Correctly formatted PS1 with non-printing segments \[ \]
FIXED_PS1="\[${BOLD}\]\[${USER_COLOR}\]\u\[${RESET_COLOR}\]@\
\[${BOLD}\]\[${DOCKER_COLOR}\]aember-container\[${RESET_COLOR}\]:\
\[${BOLD}\]\[${PATH_COLOR}\]\w\[${RESET_COLOR}\]\
\[${COMMAND_COLOR}\]\$ \[${RESET_COLOR}\]"

# --- Update .bashrc ---
cat <<EOF >> "$BASHRC"

# BEGIN AEMBER PROMPT
# Add /venv/bin to PATH if missing
if [[ ":\$PATH:" != *":/venv/bin:"* ]]; then
    export PATH="/venv/bin:\$PATH"
fi

# Suppress (venv) prefix in prompt
VIRTUAL_ENV_DISABLE_PROMPT=1

# Properly escaped PS1
PS1="${FIXED_PS1}"
# END AEMBER PROMPT
EOF

# --- Apply immediately ---
if [[ ":$PATH:" != *":/venv/bin:"* ]]; then
    export PATH="/venv/bin:$PATH"
fi

# Apply fixed prompt immediately as well
PS1="$FIXED_PS1"

# --- Symlink aember in /venv/bin ---
mkdir -p /venv/bin
ln -sf "$WORKSPACE_DIR/aember-cli/run.py" /venv/bin/aember
