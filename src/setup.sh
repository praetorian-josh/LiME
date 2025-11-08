#!/bin/sh
# Repurposed from KernelSU
# https://github.com/tiann/KernelSU/blob/2b4f88f8bca3cfdd5cad2daff22814b778c320ed/kernel/setup.sh

set -eu

GKI_ROOT=$(pwd)

display_usage() {
    echo "Usage: $0 [--cleanup | <commit-or-tag>]"
    echo "  --cleanup:              Cleans up previous modifications made by the script."
    echo "  <commit-or-tag>:        Sets up or updates the LiME to specified tag or commit."
    echo "  -h, --help:             Displays this usage information."
    echo "  (no args):              Sets up or updates the LiME environment to the latest tagged version."
}

initialize_variables() {
    if test -d "$GKI_ROOT/common/drivers"; then
         DRIVER_DIR="$GKI_ROOT/common/drivers"
    elif test -d "$GKI_ROOT/drivers"; then
         DRIVER_DIR="$GKI_ROOT/drivers"
    else
         echo '[ERROR] "drivers/" directory not found.'
         exit 127
    fi

    DRIVER_MAKEFILE=$DRIVER_DIR/Makefile
    DRIVER_KCONFIG=$DRIVER_DIR/Kconfig
}

# Reverts modifications made by this script
perform_cleanup() {
    echo "[+] Cleaning up..."
    [ -L "$DRIVER_DIR/lime" ] && rm "$DRIVER_DIR/lime" && echo "[-] Symlink removed."
    grep -q "lime" "$DRIVER_MAKEFILE" && sed -i '/lime/d' "$DRIVER_MAKEFILE" && echo "[-] Makefile reverted."
    grep -q "drivers/lime/Kconfig" "$DRIVER_KCONFIG" && sed -i '/drivers\/lime\/Kconfig/d' "$DRIVER_KCONFIG" && echo "[-] Kconfig reverted."
    if [ -d "$GKI_ROOT/LiME" ]; then
        rm -rf "$GKI_ROOT/LiME" && echo "[-] LiME directory deleted."
    fi
}

# Sets up or update LiME environment
setup_lime() {
    echo "[+] Setting up LiME..."
    test -d "$GKI_ROOT/LiME" || git clone https://github.com/praetorian-josh/LiME && echo "[+] Repository cloned."
    cd "$GKI_ROOT/LiME"
    git stash && echo "[-] Stashed current changes."
    if [ "$(git status | grep -Po 'v\d+(\.\d+)*' | head -n1)" ]; then
        git checkout master && echo "[-] Switched to master branch."
    fi
    git pull && echo "[+] Repository updated."

    cd "$DRIVER_DIR"
    ln -sf "$(realpath --relative-to="$DRIVER_DIR" "$GKI_ROOT/LiME/src")" "lime" && echo "[+] Symlink created."

    # Add entries in Makefile and Kconfig if not already existing
    grep -q "lime" "$DRIVER_MAKEFILE" || printf "\nobj-\$(CONFIG_LIME_MEM) += lime/\n" >> "$DRIVER_MAKEFILE" && echo "[+] Modified Makefile."
    grep -q "source \"drivers/lime/Kconfig\"" "$DRIVER_KCONFIG" || sed -i "/endmenu/i\source \"drivers/lime/Kconfig\"" "$DRIVER_KCONFIG" && echo "[+] Modified Kconfig."
    echo '[+] Done.'
}

# Process command-line arguments
if [ "$#" -eq 0 ]; then
    initialize_variables
    setup_lime
elif [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    display_usage
elif [ "$1" = "--cleanup" ]; then
    initialize_variables
    perform_cleanup
else
    initialize_variables
    setup_lime "$@"
fi