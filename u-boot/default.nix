# U-Boot: the pinned release, patches/ applied in order and tree/ copied over it, the signature key's DT include,
# built from the board's defconfig and config fragments. Each has u-boot.bin, its control DT, the resolved .config
# and the build ID, which hashes every input by name (under uboot64/ for the 64-bit one, so the two never share one).
{
  pkgs,
  upstream,
  aarch64,
  board,
}:
let
  inherit (pkgs) lib;

  ours = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./tree
      ./patches
    ];
  };

  # The pinned release ($src) in ./S with patches/ applied in order and tree/ copied over it ($T); it ends in S.
  unpack = lib.removeSuffix "\n" ''
    mkdir S && tar -xf $src -C S --strip-components=1 && cd S
    # Reproducible: the release's own date in the version string.
    export SOURCE_DATE_EPOCH=$(stat -c %Y Makefile)
    # Patches edit upstream files; the overlay's are whole: a patch to one would be overwritten.
    overlay=$( (cd $T/tree && find . -type f | sed 's|^\./||')
      printf '%s\n' arch/arm/dts/ax620e-edge-key.dtsi include/loader-build-id.h)
    for p in $T/patches/*.patch; do
      for f in $(sed -n 's|^+++ b/||p' $p | cut -f1); do
        if grep -qxF "$f" <<<"$overlay"; then echo "$p: edits $f, which a whole file replaces"; exit 1; fi
      done
      patch -p1 -s --no-backup-if-mismatch -r - < $p
    done
    cp -r $T/tree/. . && chmod -R u+w .
  '';

  mkUboot =
    {
      name,
      defconfig,
      cc,
      idDir ? "",
      # The signature key's DT include; by default none.
      keyDtsi ? pkgs.writeText "key.dtsi" "/ { };\n",
      # Config fragments of tree/configs/, merged in order.
      fragments ? [ ],
      # The security engine's firmware, built in for its RNG; without it, AX620E_CE_RNG is off.
      ceFw ? null,
    }:
    pkgs.stdenv.mkDerivation {
      inherit name ours keyDtsi;
      src = upstream.u-boot-src;
      fragments = lib.concatMapStrings (f: f + "\n") fragments;
      passAsFile = [ "fragments" ];
      nativeBuildInputs = [
        cc
        pkgs.bc
        pkgs.bison
        pkgs.flex
        pkgs.openssl
        pkgs.python3
      ];
      hardeningDisable = [ "all" ];
      dontUnpack = true;
      dontConfigure = true;
      buildPhase = ''
        T=$ours
        # The ID of these exact inputs, by these names.
        ln -s $src u-boot-2026.07.tar.bz2
        ${lib.optionalString (idDir != "") "mkdir ${idDir}"}
        ln -s $keyDtsi ${idDir}key.dtsi
        cp $fragmentsPath ${idDir}fragments
        ${lib.optionalString (ceFw != null) "ln -s ${ceFw} ${idDir}ce-fw.bin"}
        build_id=$( (sha256sum u-boot-2026.07.tar.bz2 ${idDir}key.dtsi ${idDir}fragments ${
          lib.optionalString (ceFw != null) "${idDir}ce-fw.bin"
        }
          cd $ours && find . -type f -print0 | sort -z | xargs -0 sha256sum) |
          sha256sum | cut -d' ' -f1)

        ${unpack}
        cp $keyDtsi arch/arm/dts/ax620e-edge-key.dtsi
        printf '#define LOADER_BUILD_ID "%s"\n' $build_id > include/loader-build-id.h
        ${lib.optionalString (ceFw != null) "cp ${ceFw} arch/arm/mach-axera/ce-fw.bin"}
        patchShebangs --build scripts tools arch/arm

        m() { make -s O=out ARCH=arm CROSS_COMPILE=${cc.targetPrefix} -j$NIX_BUILD_CORES "$@"; }
        m ${defconfig} 2>&1 | tee build.log
        m savedefconfig 2>&1 | tee -a build.log
        diff -u $T/tree/configs/${defconfig} out/defconfig ||
          { echo "${defconfig}: not what savedefconfig makes of it (+ above)"; exit 1; }
        ${lib.optionalString (fragments != [ ]) "m ${toString fragments} 2>&1 | tee -a build.log"}
        ${lib.optionalString (ceFw == null) ''
          if grep -qx CONFIG_AX620E_CE_RNG=y out/.config; then
            sed -i 's/^CONFIG_AX620E_CE_RNG=y$/# CONFIG_AX620E_CE_RNG is not set/' out/.config
            m olddefconfig 2>&1 | tee -a build.log
          fi
        ''}
        m u-boot.bin 2>&1 | tee -a build.log
        # What we added or patched must not warn.
        ours=$( (cd $T/tree && find . -type f | sed 's|^\./||'); sed -n 's|^+++ b/||p' $T/patches/*.patch)
        if grep -i warning build.log | grep -F "$ours"; then echo "warnings in our U-Boot sources (above)"; exit 1; fi

        # The control DT and the build ID are in the image.
        cmp <(tail -c $(stat -c %s out/dts/dt.dtb) out/u-boot.bin) out/dts/dt.dtb
        grep -qaF $build_id out/u-boot.bin
      '';
      installPhase = ''
        install -Dm644 out/u-boot.bin -t $out
        install -Dm644 out/dts/dt.dtb $out/control.dtb
        install -Dm644 out/.config $out/build.config
        echo $build_id > $out/build-id
      '';
    };
in
{
  # The 32-bit U-Boot, with nixpkgs' armv7 gcc.
  uboot =
    args:
    mkUboot (
      {
        name = "u-boot";
        defconfig = "ax620e_${board}_defconfig";
        cc = pkgs.pkgsCross.armv7l-hf-multiplatform.buildPackages.gccWithoutTargetLibc;
      }
      // args
    );
  # The AArch64 U-Boot, TF-A's BL33, with nixpkgs' aarch64 gcc.
  uboot64 =
    args:
    mkUboot (
      {
        name = "u-boot64";
        defconfig = "ax620e_${board}64_defconfig";
        cc = aarch64.bare;
        idDir = "uboot64/";
      }
      // args
    );
}
