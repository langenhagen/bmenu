#!/bin/bash
# Call bmenu on the given array of choices, effectively acting as a simple file explorer
# based on `explore-with-dmenu` (https://github.com/langenhagen/explore-with-dmenu) and reading
# its rc file from the same `${XDG_CONFIG_HOME:-$HOME/.config}/edm/edmrc` path.
#
# If the selected choice is a folder, recursively open bmenu with the folder's contents as choices.
# If the selection is not a folder - or the current folder, denoted as '.' - attempt to open it with
# xdg-open or a tool configured via .edmrc-file and write the selection to a history file.
# The script can also open a terminal at the selected path.
#
# author: andreasl

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bmenu_bin="${script_dir}/build/bmenu"

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/edm"
mkdir -p "$config_dir"

define_standard_settings() {
    selected_path="$HOME"
    history_file="${config_dir}/history"
    max_history_entries=3

    choices=(
        '<open terminal here>'
        '.'
        '..'
        "$(ls "$selected_path")"
        "$(cat "$history_file")"
    )

    if [ "$(uname)" == "Darwin" ]; then
        open_command='open'
        open_terminal_command='open -a Terminal'
    else
        open_command='xdg-open'
        open_terminal_command='gnome-terminal --working-directory'
    fi
}
define_standard_settings
source "${config_dir}/edmrc" 2>/dev/null

write_selection_to_history_file() {
    sed -i "\:${1}:d" "$history_file"
    printf '%s\n' "$1" >>"$history_file"
    printf '%s\n' "$(tail -n "$max_history_entries" "$history_file")" >"$history_file"
}

while :; do
    bmenu_result="$(printf '%s\n' "${choices[@]}" | "$bmenu_bin")" || exit 1
    if [ "$bmenu_result" == '<open terminal here>' ]; then
        $open_terminal_command "$selected_path"
        write_selection_to_history_file "$selected_path"
        exit 0
    elif [[ $bmenu_result == '/'* ]]; then
        selected_path="$bmenu_result"
    elif [[ $bmenu_result =~ ^(https?|ftps): ]]; then
        "$open_command" "$bmenu_result"
        write_selection_to_history_file "$bmenu_result"
        exit 0
    else
        selected_path="$(realpath "${selected_path}/${bmenu_result}")"
    fi

    if [ -f "$selected_path" ] || [ "$bmenu_result" = '.' ]; then
        "$open_command" "$selected_path"
        write_selection_to_history_file "$selected_path"
        exit 0
    elif [ -d "$selected_path" ]; then
        choices=('<open terminal here>' '.' '..' "$(ls "$selected_path")")
    else
        selected_path="$(dirname "$selected_path")"
    fi
done
