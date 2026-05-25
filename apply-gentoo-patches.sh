#!/bin/bash
# Reapply Gentoo/modern CMake compatibility patches after pulling upstream changes.
# Usage: ./apply-gentoo-patches.sh
set -e

REPO_ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
PATCH="$REPO_ROOT/gentoo-cmake-compat.patch"

if [ ! -f "$PATCH" ]; then
    echo "ERROR: $PATCH not found"
    exit 1
fi

cd "$REPO_ROOT"
echo "Applying Gentoo cmake compatibility patches..."
git apply --3way "$PATCH"
echo "Done."
