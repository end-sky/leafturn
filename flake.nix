{
  description = "Leafturn - a lightweight C PDF reader for Linux with book-style page turns";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system:
        f (import nixpkgs { inherit system; })
      );
    in {
      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./package.nix {};
        leafturn = pkgs.callPackage ./package.nix {};
      });

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = "${pkgs.callPackage ./package.nix {}}/bin/leafturn";
        };
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            gcc
            pkg-config
            gtk3
            (mupdf.override {
              enableCurl = false;
              enableGL = false;
              enableX11 = false;
            })
          ];
        };
      });
    };
}
