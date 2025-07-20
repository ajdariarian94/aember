setup_prompt() {
    local bashrc="$HOME/.bashrc"

    local USER_COLOR='\033[32m'
    local DOCKER_COLOR='\033[34m'
    local PATH_COLOR='\033[35m'
    local COMMAND_COLOR='\033[38;5;214m'
    local BOLD='\033[1m'
    local RESET_COLOR='\033[0m'

    {
        echo -e "\n# Custom prompt for aember"
        echo "PS1='${BOLD}\[${USER_COLOR}\]\u\[${RESET_COLOR}\]@${BOLD}\[${DOCKER_COLOR}\]aember\[${RESET_COLOR}\]:${BOLD}\[${PATH_COLOR}\]\w\[${RESET_COLOR}\]${COMMAND_COLOR}\$ ${RESET_COLOR}'"
    } >> "$bashrc"

    ln -s "$(pwd)/python/run.py" /venv/bin/aember
}