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

EOF

# Source helper scripts
for script in "$SCRIPT_DIR"/bashrc.d/*.sh; do
    [ -e "$script" ] && source "$script"
done

# Activate bashrc and virtualenv immediately
source "$HOME/.bashrc"
source /venv/bin/activate

aember --install-completion  

sudo rm -rf /root/.config/git

# -----------------------------
# Setup loop devices for pack_step
# -----------------------------
echo "[init][info] Checking loop devices..."
for i in $(seq 0 17); do
    if [ ! -e /dev/loop$i ]; then
        echo "[init][warn] /dev/loop$i missing! Host should provide loop devices."
    fi
done

if [ ! -e /dev/loop-control ]; then
    echo "[init][warn] /dev/loop-control missing! Host should provide it."
fi

echo "[init][info] Loop devices ready (host-provided)."


# -----------------------------
# Show banner
# -----------------------------
figlet -f big aember | lolcat
