# The builds, for `pkgs` (the build machine) and `devicePkgs` (aarch64-linux, what the device's NixOS system is
# made of).
{ pkgs, devicePkgs }:
let
  upstream = import ./upstream.nix { inherit (pkgs) fetchurl; };

  # AArch64 gcc on the build machine: `bare` (no libc) for the kernel and U-Boot; `cc` compiles and links against
  # the device's glibc and libgcc_s (searched first, so the runpath shrinks to them), so its programs need nothing
  # beyond what the NixOS system already has.
  aarch64 =
    let
      cross = pkgs.pkgsCross.aarch64-multiplatform.buildPackages;
      libc = devicePkgs.glibc;
    in
    {
      bare = cross.gccWithoutTargetLibc;
      cc = cross.wrapCCWith {
        inherit (cross.gcc) cc;
        inherit libc;
        bintools = cross.wrapBintoolsWith {
          bintools = cross.binutils-unwrapped;
          inherit libc;
        };
        # Ahead of the cross gcc's own library directory.
        extraBuildCommands = ''
          echo "-L${devicePkgs.stdenv.cc.cc.libgcc}/lib" >> $out/nix-support/cc-cflags-before
        '';
      };
    };
in
{
  inherit upstream aarch64;
  # Linux for a board: `{ board, fragments ? [ ], usbIds ? <the board's>, firmware ? null }`.
  linux =
    args:
    import ./linux (
      {
        inherit
          pkgs
          devicePkgs
          upstream
          aarch64
          ;
      }
      // args
    );
  # The 32- and 64-bit U-Boots for a board: `{ board }`.
  u-boot =
    args:
    import ./u-boot (
      {
        inherit pkgs upstream aarch64;
      }
      // args
    );
  fw-env = import ./fw-env.nix {
    inherit
      pkgs
      devicePkgs
      upstream
      aarch64
      ;
  };
}
