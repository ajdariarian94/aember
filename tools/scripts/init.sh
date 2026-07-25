#!/bin/bash
# Fully reset .bashrc and setup workspace environment for aember

# Prevent double execution — VS Code runs postStartCommand twice simultaneously.
# Use uptime-based stamp: if second run starts within 5s of first, skip it.
UPTIME=$(awk '{print int($1)}' /proc/uptime)
STAMP_FILE="/home/dev/.aember-init-stamp"

if [ -f "$STAMP_FILE" ]; then
    LAST=$(cat "$STAMP_FILE")
    if [ "$((UPTIME - LAST))" -lt 5 ]; then
        figlet -f big "hacking..." | lolcat
        exit 0
    fi
fi

echo "$UPTIME" > "$STAMP_FILE"

# Resolve script and workspace directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)/aember_ws/aember"

# Fix Docker GID mismatch — host GID may differ from image GID
HOST_DOCKER_GID="${DOCKER_GID:-}"
if [ -S /var/run/docker.sock ] && [ -n "$HOST_DOCKER_GID" ]; then
    CURRENT_DOCKER_GID=$(getent group docker | cut -d: -f3 2>/dev/null || echo "")
    if [ -n "$CURRENT_DOCKER_GID" ] && [ "$HOST_DOCKER_GID" != "$CURRENT_DOCKER_GID" ]; then
        sudo groupmod -g "$HOST_DOCKER_GID" docker 2>/dev/null || true
    fi
fi

# Install aember completion first so we can source it in .bashrc
aember --install-completion > /dev/null 2>&1

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

# Aember CLI completion
if [ -f ~/.bash_completions/aember.sh ]; then
    source ~/.bash_completions/aember.sh
fi

# Load secrets from .env if present (not committed to repo)
EOF

# Append WORKSPACE_DIR dynamically (can't use single-quote heredoc for variables)
cat >> "$HOME/.bashrc" << EOF
ENV_FILE="$WORKSPACE_DIR/tools/scripts/.env"
if [ -f "\$ENV_FILE" ]; then
    source "\$ENV_FILE"
fi

# Authenticate gh CLI if token is available
if [ -n "\$GITHUB_TOKEN" ]; then
    echo "\$GITHUB_TOKEN" | gh auth login --with-token 2>/dev/null
fi
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
