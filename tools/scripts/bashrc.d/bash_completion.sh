#!/bin/bash
# Enable bash-completion (idempotent)

BASHRC="$HOME/.bashrc"

if ! grep -q "# BEGIN AEMBER BASH COMPLETION" "$BASHRC"; then
cat <<'EOF' >> "$BASHRC"

# BEGIN AEMBER BASH COMPLETION
if [ -f /etc/bash_completion ]; then
   . /etc/bash_completion
fi
# END AEMBER BASH COMPLETION
EOF
fi

# Enable bash-completion immediately in current shell
if [ -f /etc/bash_completion ]; then
    . /etc/bash_completion
fi
