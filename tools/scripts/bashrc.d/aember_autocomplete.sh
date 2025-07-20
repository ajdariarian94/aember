aember_autocomplete() {
    local autocomplete_script="$(pwd)/python/autocomplete/autocomplete_aember.py"
    local bashrc="$HOME/.bashrc"

    {
        echo -e "\n# Autocomplete function for aember"
        echo "_autocomplete_aember() {"
        echo "    local cur prev words cword"
        echo "    _init_completion || return"
        echo "    COMPREPLY=(\$(python3 $autocomplete_script \"\${words[@]:1}\"))"
        echo "}"
        echo "complete -F _autocomplete_aember aember"
        echo "export CI=true"
    } >> "$bashrc"
}