bash_completion() {
    {
        echo -e "\n# Enable bash-completion"
        echo "if [ -f /etc/bash_completion ]; then"
        echo "   . /etc/bash_completion"
        echo "fi"
    } >> "$HOME/.bashrc"
}
