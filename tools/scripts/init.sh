#!/bin/bash

# Resolve the directory this script lives in
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source all helper scripts
for script in "$SCRIPT_DIR"/bashrc.d/*.sh; do
    [ -e "$script" ] && source "$script"
done

bash_completion
aember_autocomplete
setup_prompt

source ~/.bashrc
source /venv/bin/activate

figlet -f big aember | lolcat

bash