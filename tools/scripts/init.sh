#!/bin/bash
# Fully reset .bashrc and setup workspace environment for aember

# Prevent double execution — VS Code runs postStartCommand twice simultaneously.
# Use a lock file under /run (tmpfs) rather than /home/dev: /run is guaranteed
# to be cleared on every container restart, so a stale lock can never survive
# across restarts and skip re-initialization (which previously caused the
# Docker GID fix to be silently skipped after a container restart, since
# /proc/uptime resets but a stamp file under /home/dev does not).
LOCK_FILE="/run/aember-init.lock"

if [ -e "$LOCK_FILE" ]; then
    figlet -f big "hacking..." | lolcat
    exit 0
fi

touch "$LOCK_FILE"

# Resolve script and workspace directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)/aember_ws/aember"

# -----------------------------
# Fix Docker GID mismatch and verify socket access
# -----------------------------
# The image's built-in "docker" group GID may not match the host's docker
# group GID (the socket is bind-mounted in with the HOST's GID as owner).
# Rather than renumbering the existing "docker" group — which can fail if
# that GID collides with an unrelated group, or break anything else already
# tied to the old GID — create a dedicated group at the host's GID and add
# "dev" to it. Group membership on the socket only cares about GID, not name,
# so "dev" being in either group is sufficient.
HOST_DOCKER_GID="${DOCKER_GID:-}"

if [ -S /var/run/docker.sock ]; then
    # Prefer the actual socket owner GID if DOCKER_GID wasn't passed through
    if [ -z "$HOST_DOCKER_GID" ]; then
        HOST_DOCKER_GID=$(stat -c '%g' /var/run/docker.sock 2>/dev/null || echo "")
    fi

    if [ -n "$HOST_DOCKER_GID" ]; then
        # Does a group with this GID already exist (could be "docker" itself,
        # or a "docker-host" group from a previous run)?
        EXISTING_GROUP=$(getent group "$HOST_DOCKER_GID" | cut -d: -f1 || echo "")

        if [ -z "$EXISTING_GROUP" ]; then
            sudo groupadd -g "$HOST_DOCKER_GID" docker-host 2>/dev/null || true
            EXISTING_GROUP="docker-host"
        fi

        if ! id -nG dev 2>/dev/null | grep -qw "$EXISTING_GROUP"; then
            sudo usermod -aG "$EXISTING_GROUP" dev 2>/dev/null || true
        fi
    fi

    # 🎩 Cheat code: grant dev a direct ACL on the socket itself. This
    # bypasses the group-membership/login-session timing problem entirely —
    # usermod only takes effect in a *new* login session, but setfacl takes
    # effect immediately, even for shells that are already open. Requires
    # the `acl` package (setfacl) to be present in the image.
    if command -v setfacl >/dev/null 2>&1; then
        sudo setfacl -m u:dev:rw /var/run/docker.sock 2>/dev/null || true
    else
        echo "⚠️  setfacl not found (install 'acl' package) — falling back to group membership only" >&2
    fi

    # Verify the socket is actually reachable as dev
    if ! sudo -u dev docker info >/dev/null 2>&1; then
        echo "⚠️  dev user still cannot reach /var/run/docker.sock — Docker-in-Docker builds will fail" >&2
        echo "    (open a fresh shell if this was just fixed — group membership needs a new session)" >&2
    fi
else
    echo "⚠️  /var/run/docker.sock not found — is the socket mounted into this container?" >&2
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
