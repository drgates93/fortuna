#!/bin/bash

YELLOW='\033[0;33m'
GREEN='\033[0;32m'
RESET='\033[0m'

DIR=$(pwd)
SHELL_CONFIG=""

# Detect shell config file
if [ -n "$ZSH_VERSION" ]; then
    SHELL_CONFIG="$HOME/.zshrc"
elif [ -n "$BASH_VERSION" ]; then
    SHELL_CONFIG="$HOME/.bashrc"
elif [ -f "$HOME/.profile" ]; then
    SHELL_CONFIG="$HOME/.profile"
fi

echo -e "${YELLOW}Adding directory:${RESET} $DIR"
echo -e "${YELLOW}Shell config:${RESET} $SHELL_CONFIG"

export DIR

if grep -Fxq "export PATH=\"$DIR:\$PATH\"" "$SHELL_CONFIG"; then
    echo -e "${GREEN}Already in PATH.${RESET}"
else
    echo "export PATH=\"$DIR:\$PATH\"" >> "$SHELL_CONFIG"
    echo -e "${GREEN}Added to PATH. Restart your terminal or run:${RESET}"
    echo -e "export PATH=\"$DIR:\$PATH\""
fi