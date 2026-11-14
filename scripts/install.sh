#!/bin/sh
# Install the latest AsciiCam release for current user:
#   curl -fsSL https://raw.githubusercontent.com/Harshit-Dhanwalkar/AsciiCam/main/scripts/install.sh | sh
# Environment: PREFIX (default ~/.local), VERSION (default latest, e.g. v0.1.0)
set -eu

repo=Harshit-Dhanwalkar/AsciiCam
prefix=${PREFIX:-$HOME/.local}

[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] || {
  echo "asciicam release binaries are x86-64 Linux only; build from source instead" >&2
  exit 1
}
fetch() { # url [file]
  if command -v curl >/dev/null; then curl -fsSL ${2:+-o "$2"} "$1"
  else wget -qO "${2:--}" "$1"; fi
}

tag=${VERSION:-}
if [ -z "$tag" ]; then
  tag=$(fetch "https://api.github.com/repos/$repo/releases/latest" |
    sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n1)
fi
[ -n "$tag" ] || { echo "cannot find the latest release" >&2; exit 1; }

name=asciicam-${tag#v}-linux-x86_64
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
base=https://github.com/$repo/releases/download/$tag
fetch "$base/$name.tar.gz" "$tmp/$name.tar.gz"
fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS"
(cd "$tmp" && grep " $name.tar.gz\$" SHA256SUMS | sha256sum -c -) >&2

tar -xzf "$tmp/$name.tar.gz" -C "$tmp"
mkdir -p "$prefix"
cp -R "$tmp/$name/." "$prefix/"
echo "installed asciicam $tag to $prefix/bin/asciicam"
case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "add $prefix/bin to your PATH" ;; esac
