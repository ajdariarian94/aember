#!/usr/bin/env bash
# chroot_test_runner.sh
# Usage: chroot_test_runner.sh <chroot_path> <test_binary> [args...]

set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <chroot_path> <test_binary> [args...]"
    exit 1
fi

CHROOT_PATH="$1"
TEST_BINARY="$2"
shift 2
TEST_ARGS=("$@")

# Resolve absolute path inside chroot
REL_TEST_PATH="${TEST_BINARY#$CHROOT_PATH/}"
ABS_TEST_PATH="/$REL_TEST_PATH"

echo "Running test inside chroot: $ABS_TEST_PATH ${TEST_ARGS[*]}"

# Mount /proc and /dev if not already mounted
if ! mountpoint -q "$CHROOT_PATH/proc"; then
    sudo mount -t proc proc "$CHROOT_PATH/proc"
fi

if ! mountpoint -q "$CHROOT_PATH/dev"; then
    sudo mount --bind /dev "$CHROOT_PATH/dev"
fi

# Run the test inside chroot
sudo chroot "$CHROOT_PATH" "$ABS_TEST_PATH" "${TEST_ARGS[@]}"
EXIT_CODE=$?

# Optional: unmount /proc and /dev if you want cleanup after each test
sudo umount "$CHROOT_PATH/proc"
sudo umount "$CHROOT_PATH/dev"

exit $EXIT_CODE
