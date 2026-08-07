#!/bin/sh
# Builds Forge from source and installs it to a user-level bin directory.
# Usage: ./scripts/install.sh
#
# Honours CC (or the first available of gcc/clang) and
# XDG_BIN_HOME / HOME for the install location.

set -e

root="$(cd "$(dirname "$0")/.." && pwd)"
prefix="${XDG_BIN_HOME:-$HOME/.local/bin}"
target_dir="$root/target"
exe="$target_dir/forge"
installed="$prefix/forge"

if command -v gcc >/dev/null 2>&1; then
    compiler="${CC:-gcc}"
elif command -v clang >/dev/null 2>&1; then
    compiler="${CC:-clang}"
else
    echo "error: gcc or clang is required to build Forge" >&2
    exit 1
fi

mkdir -p "$target_dir" "$prefix"
$compiler -Wall -Wextra -Werror -std=c11 -I"$root/include" "$root"/src/*.c -o "$exe"
cp "$exe" "$installed"
chmod +x "$installed"

echo "Installed forge to $installed"
echo ""
echo "To use 'forge' from any directory, add the bin folder to your PATH:"
echo ""
echo "    export PATH=\"$prefix:\$PATH\""
echo ""
echo "Verify with:  forge --help"