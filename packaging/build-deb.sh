#!/bin/sh
# Build asciicam_<version>_amd64.deb with plain dpkg-deb
# Run : packaging/build-deb.sh [version]  ->  dist/asciicam_*.deb

set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
ver=${1:-$(sed -n '1s/.*(\(.*\)-1).*/\1/p' "$here/debian/changelog")}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
chmod 755 "$stage"

make -C "$root/C" clean >/dev/null
make -C "$root/C" install PREFIX=/usr DESTDIR="$stage" >/dev/null
install -d "$stage/usr/share/doc/asciicam" "$stage/DEBIAN"
install -m 644 "$root/LICENCE" "$stage/usr/share/doc/asciicam/LICENCE"
install -m 644 "$here/debian/copyright" "$stage/usr/share/doc/asciicam/copyright"
gzip -9n -c "$here/debian/changelog" >"$stage/usr/share/doc/asciicam/changelog.gz"

size=$(du -sk "$stage/usr" | cut -f1)
sed -n '/^Package:/,$p' "$here/debian/control" |
  sed -e '/^Depends:/d' -e "/^Architecture:/a Version: $ver-1\nInstalled-Size: $size" \
    >"$stage/DEBIAN/control"
sed -i '/^Rules-Requires-Root/d' "$stage/DEBIAN/control"
sed -i "/^Description:/i Maintainer: Harshit Dhanwalkar <harshitpd1729@gmail.com>\nSection: video\nPriority: optional\nHomepage: https://github.com/Harshit-Dhanwalkar/AsciiCam" "$stage/DEBIAN/control"

mkdir -p "$root/dist"
out="$root/dist/asciicam_${ver}-1_amd64.deb"
dpkg-deb --root-owner-group --build "$stage" "$out" >/dev/null
echo "$out"
