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
echo "to start it with every terminal: $repo#put-it-in-your-shell"
echo "to remove it later: ffanim --uninstall"
case ":$PATH:" in
*":$prefix/bin:"*) ;;
*) echo "add $prefix/bin to your PATH" ;;
esac
