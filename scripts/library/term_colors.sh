# check if messages are to be printed using color
unset ALL_OFF BOLD BLUE GREEN RED YELLOW
if [[ -t 2 && ! $USE_COLOR = "n" ]]; then
	# prefer terminal safe colored and bold text when tput is supported
	if /bin/tput setaf 0 &>/dev/null; then
		ALL_OFF="$(/usr/bin/tput sgr0)"
		BOLD="$(/usr/bin/tput bold)"
		BLUE="${BOLD}$(/usr/bin/tput setaf 4)"
		GREEN="${BOLD}$(/usr/bin/tput setaf 2)"
		RED="${BOLD}$(/usr/bin/tput setaf 1)"
		YELLOW="${BOLD}$(/usr/bin/tput setaf 3)"
	else
		ALL_OFF="\e[1;0m"
		BOLD="\e[1;1m"
		BLUE="${BOLD}\e[1;34m"
		GREEN="${BOLD}\e[1;32m"
		RED="${BOLD}\e[1;31m"
		YELLOW="${BOLD}\e[1;33m"
	fi
fi
readonly ALL_OFF BOLD BLUE GREEN RED YELLOW
