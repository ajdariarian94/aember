#!/bin/bash
# Setup autocomplete for aember (idempotent)

BASHRC="$HOME/.bashrc"
WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)/aember_ws"
AUTOCOMPLETE_SCRIPT="$WORKSPACE_DIR/python/autocomplete/autocomplete_aember.py"

# Append to .bashrc once
if ! grep -q "# BEGIN AEMBER AUTOCOMPLETE" "$BASHRC"; then
cat <<EOF >> "$BASHRC"

# BEGIN AEMBER AUTOCOMPLETE
_autocomplete_aember() {
    local cur prev words cword
    _init_completion || return
    COMPREPLY=(\$(python3 "$AUTOCOMPLETE_SCRIPT" "\${words[@]:1}"))
}
complete -F _autocomplete_aember aember
export CI=true
# END AEMBER AUTOCOMPLETE
EOF
fi

# Register autocomplete immediately in current shell
_autocomplete_aember() {
    local cur prev words cword
    _init_completion || return
    COMPREPLY=($(python3 "$AUTOCOMPLETE_SCRIPT" "${words[@]:1}"))
}
complete -F _autocomplete_aember aember
export CI=true
