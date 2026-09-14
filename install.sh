#!/bin/sh
set -eu

repo=https://github.com/ihsan-ramadhan/ffanim
ref=${FFANIM_REF:-main}
prefix=${PREFIX:-$HOME/.local}

for c in curl tar make cc; do
    command -v "$c" >/dev/null || { echo "ffanim: needs $c" >&2; exit 1; }
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

curl -fsSL "$repo/archive/$ref.tar.gz" | tar xz -C "$tmp" --strip-components 1
make -s -C "$tmp" install PREFIX="$prefix"

echo "ffanim installed to $prefix/bin/ffanim"
if [ -n "${FFANIM_NO_SETUP:-}" ]; then
    echo "shell setup skipped: run ffanim --setup when you want it"
else
    "$prefix/bin/ffanim" --setup
fi
echo "to remove it later: ffanim --uninstall"
case ":$PATH:" in
*":$prefix/bin:"*) ;;
*) echo "add $prefix/bin to your PATH" ;;
esac
