{
  description = "AsciiCam - dependency-free webcam to ASCII renderer for the terminal";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      # Binary is static x86-64 Linux with raw syscalls (no libc)
      systems = [ "x86_64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      packages = forAll (pkgs: rec {
        asciicam = pkgs.stdenv.mkDerivation {
          pname = "asciicam";
          version = "0.1.0";
          src = ./.;
          sourceRoot = "source/C";

          # No libc is linked; tests run in sandbox without a camera
          hardeningDisable = [ "all" ];
          makeFlags = [ "PREFIX=${placeholder "out"}" ];
          installTargets = "install";
          doCheck = true;
          checkTarget = "test";

          meta = {
            description = "Webcam to ASCII art in the terminal, no libc and no dependencies";
            homepage = "https://github.com/Harshit-Dhanwalkar/AsciiCam";
            license = pkgs.lib.licenses.polyform-noncommercial-100;
            platforms = systems;
            mainProgram = "asciicam";
          };
        };
        default = asciicam;
      });

      apps = forAll (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.system}.asciicam}/bin/asciicam";
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [ gcc gnumake clang clang-tools python3 ];
        };
      });
    };
}
