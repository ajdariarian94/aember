#!/bin/bash
# Fully reset .bashrc and setup workspace environment for aember
# Also ensures loop devices are ready inside Docker

# Resolve script and workspace directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)/aember_ws/aember"

# -----------------------------
# Reset .bashrc completely
# -----------------------------
cat > "$HOME/.bashrc" <<'EOF'
# ~/.bashrc: reset by aember init.sh
# Only interactive shells
case $- in
    *i*) ;;
      *) return;;
esac

# History settings
HISTCONTROL=ignoreboth
shopt -s histappend
HISTSIZE=1000
HISTFILESIZE=2000
shopt -s checkwinsize

# Basic aliases
alias ll='ls -alF'
alias la='ls -A'
alias l='ls -CF'
alias alert='notify-send --urgency=low -i "$([ $? = 0 ] && echo terminal || echo error)" "$(history|tail -n1|sed -e '\''s/^\s*[0-9]\+\s*//;s/[;&|]\s*alert$//'\'')"'

# Git alias to run as dev user automatically
alias git='sudo -E -u dev git'

export HOME=/home/dev

# Enable bash-completion
if [ -f /etc/bash_completion ]; then
    . /etc/bash_completion
fi

# Load secrets from .env if present (not committed to repo)
ENV_FILE="$WORKSPACE_DIR/tools/scripts/.env"
if [ -f "$ENV_FILE" ]; then
    source "$ENV_FILE"
fi

# GitHub token for private repo artifact downloads
# Remove when repo goes public before CppCon
export GITHUB_TOKEN="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"

# Authenticate gh CLI if token is available
if [ -n "$GITHUB_TOKEN" ]; then
    echo "$GITHUB_TOKEN" | gh auth login --with-token 2>/dev/null
fi

aember --install-completion > /dev/null 2>&1

EOF

# Source helper scripts
for script in "$SCRIPT_DIR"/bashrc.d/*.sh; do
    [ -e "$script" ] && source "$script"
done

# Activate bashrc
source "$HOME/.bashrc"

sudo rm -rf /root/.config/git

# -----------------------------
# Show banner
# -----------------------------
figlet -f big aember | lolcat
