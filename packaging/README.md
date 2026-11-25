# Packaging and distribution

Everything here installs : `make -C C install PREFIX=...` (binary, prebuilt plugins, charsets).
The binary is static x86-64 Linux with no libc, so one build runs on any distribution.

| Method                      | Files                                                 | Status                                                              |
| --------------------------- | ----------------------------------------------------- | ------------------------------------------------------------------- |
| Release tarball + installer | `scripts/install.sh`, `.github/workflows/release.yml` | installer tested against a local fake release; workflow not run yet |
| `.deb`                      | `packaging/build-deb.sh`, `packaging/debian/`         | built, `dpkg -i`, run and `dpkg -r` tested on Debian-family         |
| Nix flake                   | `flake.nix` (repo root)                               | syntax checked only (nixpkgs unreachable in the build sandbox)      |
| Homebrew tap                | `packaging/homebrew/asciicam.rb`                      | not run; needs a tag and sha256                                     |
| Arch (AUR)                  | `packaging/arch/PKGBUILD`                             | not run                                                             |
| Docker                      | `Dockerfile`                                          | existing                                                            |

## Users

    curl -fsSL https://raw.githubusercontent.com/Harshit-Dhanwalkar/AsciiCam/main/scripts/install.sh | sh
    sudo apt install ./asciicam_0.3.1-1_amd64.deb
    nix run github:Harshit-Dhanwalkar/AsciiCam            # needs allowUnfree, see licence note
    brew install Harshit-Dhanwalkar/asciicam/asciicam     # tap repo: homebrew-asciicam
    yay -S asciicam                                       # after the AUR package is submitted
