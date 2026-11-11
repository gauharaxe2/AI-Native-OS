# Sourced from /etc/profile for login shells.
if [ -n "$PS1" ]; then
    export PS1='ai-native-os:\w# '
    echo "Type 'aios status' or 'aios audit list'. Halt with 'poweroff -f'."
fi
