{
  description = "GembaHive OS: GPL and LGPL sources";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      # The build machine; the kernel, U-Boot and fw_printenv cross-compile from it.
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      # The device's own packages, of which its NixOS system is made.
      devicePkgs = nixpkgs.legacyPackages.aarch64-linux;
      gpl = self.lib.mkGpl { inherit pkgs devicePkgs; };
      board = "edge";
      linux = gpl.linux { inherit board; };
      u-boot = gpl.u-boot { inherit board; };
    in
    {
      lib.mkGpl = import ./.;
      nixosModules.default = ./nixos;

      packages.${system} = gpl.upstream // {
        kernel = linux.build;
        uboot = u-boot.uboot { };
        uboot64 = u-boot.uboot64 { };
        inherit (gpl) fw-env;
        # A NixOS system with the package overrides, each in its closure.
        toplevel =
          (nixpkgs.lib.nixosSystem {
            modules = [
              { nixpkgs.pkgs = devicePkgs; }
              self.nixosModules.default
              (
                { gplPkgs, ... }:
                {
                  boot.kernelPackages = linux.nixosPackages;
                  boot.initrd.includeDefaultModules = false;
                  boot.initrd.allowMissingModules = true;
                  boot.loader.grub.enable = false;
                  fileSystems."/" = {
                    device = "none";
                    fsType = "tmpfs";
                  };
                  services.chrony.enable = true;
                  networking.nftables.enable = true;
                  environment.systemPackages = [
                    gplPkgs.iproute2
                    gplPkgs.e2fsprogs
                  ];
                  system.stateVersion = "26.05";
                }
              )
            ];
          }).config.system.build.toplevel;
      };

      # Format and lint checks for the Nix files.
      checks.${system}.nix-lint =
        pkgs.runCommand "nix-lint"
          {
            src = pkgs.lib.fileset.toSource {
              root = ./.;
              fileset = pkgs.lib.fileset.unions [
                ./statix.toml
                (pkgs.lib.fileset.fileFilter (f: f.hasExt "nix") ./.)
              ];
            };
            nativeBuildInputs = [
              pkgs.nixfmt
              pkgs.statix
              pkgs.deadnix
            ];
          }
          ''
            cd $src
            nixfmt --check $(find . -name '*.nix')
            statix check .
            deadnix --fail .
            touch $out
          '';
      formatter.${system} = pkgs.nixfmt-tree;
    };
}
