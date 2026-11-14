#!/bin/sh
set -eu
key=${1:?usage: make-apt-repo.sh <gpg-key-id>}
root=$(cd "$(dirname "$0")/.." && pwd)
repo=$root/apt
suite=stable

rm -rf "$repo"
mkdir -p "$repo/pool/main" "$repo/dists/$suite/main/binary-amd64"
cp "$root"/dist/*.deb "$repo/pool/main/"

cd "$repo"
dpkg-scanpackages --arch amd64 pool >"dists/$suite/main/binary-amd64/Packages"
gzip -9nk "dists/$suite/main/binary-amd64/Packages"
apt-ftparchive \
  -o APT::FTPArchive::Release::Origin=AsciiCam \
  -o APT::FTPArchive::Release::Label=AsciiCam \
  -o APT::FTPArchive::Release::Suite=$suite \
  -o APT::FTPArchive::Release::Codename=$suite \
  -o APT::FTPArchive::Release::Architectures=amd64 \
  -o APT::FTPArchive::Release::Components=main \
  release "dists/$suite" >"dists/$suite/Release"

gpg --batch --yes --default-key "$key" -abs -o "dists/$suite/Release.gpg" "dists/$suite/Release"
gpg --batch --yes --default-key "$key" --clearsign -o "dists/$suite/InRelease" "dists/$suite/Release"
gpg --export "$key" | gpg --dearmor >"$repo/asciicam.gpg" 2>/dev/null || gpg --export "$key" >"$repo/asciicam.gpg"
echo "repository written to $repo (publish this directory)"
