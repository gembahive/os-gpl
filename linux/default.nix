# Linux cross-built (ARCH=arm64) with nixpkgs' aarch64 gcc, split so initramfs changes do not recompile it:
#   src    the release, patches/ applied in order, tree/ and the board's files over it, the firmware and the signed
#          regulatory database
#   build  everything compiled (defconfig + nixos.config + the selected fragments) against a placeholder initramfs,
#          the DTB, the W=1 reports
#   image  build's objects with an initramfs (a gen_init_cpio list or a NixOS initrd): make relinks only usr/ and
#          vmlinux
{
  pkgs,
  # NixOS's kernelPackages are the device's.
  devicePkgs,
  upstream,
  aarch64,
  board,
  # Config fragments of <board>/ merged after nixos.config, in order: debug-logs.config, verbose-boot.config,
  # driver-modules.config, usb-hid.config, no-boot-lock.config, serial-console.config.
  fragments ? [ ],
  # Merged last: the vendor:product IDs a configfs gadget may bind with.
  usbIds ? ./. + "/${board}/usb-ids.config",
  # A directory of the files CONFIG_EXTRA_FIRMWARE builds in. Without it, only the regulatory database is built in.
  firmware ? null,
}:
let
  inherit (pkgs) lib stdenv;
  fs = lib.fileset;
  boardDir = ./. + "/${board}";
  # Kbuild ARCH, for the aarch64 cross compiler.
  kbuild = "arm64";
  dtb = "axera/${board}.dtb";

  # Kbuild environment, shared by build and image so the relink sees identical commands. Fixed build date and
  # identity for reproducibility.
  kernelEnv = ''
    export SOURCE_DATE_EPOCH=$(cat ${src}/source-date-epoch)
    export KBUILD_BUILD_TIMESTAMP=$(date -u -R -d "@$SOURCE_DATE_EPOCH")
    export KBUILD_BUILD_USER=root KBUILD_BUILD_HOST=gemba KBUILD_BUILD_VERSION=1
    S=${src}/src
    # Same absolute path in build and image, so image reuses build's objects.
    O=$NIX_BUILD_TOP/o
    k() { make -s -C "$S" ARCH=${kbuild} CROSS_COMPILE=${aarch64.bare.targetPrefix} -j"$NIX_BUILD_CORES" "$@"; }
    m() { k O="$O" "$@"; }
  '';
  kernelDrv =
    args:
    stdenv.mkDerivation (
      {
        nativeBuildInputs = with pkgs; [
          aarch64.bare
          bc
          bison
          flex
          kmod
          openssl
          perl
          python3
        ];
        # The kernel sets its own hardening flags.
        hardeningDisable = [ "all" ];
        # Else stdenv adds an rpath of $out/lib to every link (the vDSO too), putting a store path in the Image.
        NIX_NO_SELF_RPATH = true;
        # Fixed -frandom-seed (the default derives from $out), so build and image compile alike.
        NIX_OUTPATH_USED_AS_RANDOM_SEED = "linux-${board}";
      }
      // args
    );

  src = stdenv.mkDerivation {
    name = "linux-${board}-src";
    src = fs.toSource {
      root = ./.;
      fileset = fs.unions [
        ./patches
        ./tree
        boardDir
      ];
    };
    # Source path and tree path of each board file laid over the patched release.
    overlay = ''
      ${board}/arm64.defconfig arch/${kbuild}/configs/${board}_defconfig
      ${board}/${board}.dts arch/arm/boot/dts/axera/${board}.dts
      ${lib.concatMapStrings (f: "${board}/${f} arch/${kbuild}/configs/${board}-${f}\n") (
        lib.filter (lib.hasSuffix ".config") (lib.attrNames (builtins.readDir boardDir))
      )}'';
    buildCommand = ''
      cd $src
      mkdir -p $out/src
      tar -xf ${upstream.linux-src} -C $out/src --strip-components=1
      stat -c %Y $out/src/Makefile > $out/source-date-epoch
      overlay=$(find tree -type f | sort | sed 's|^tree/\(.*\)|tree/\1 \1|'; echo -n "$overlay")
      # Patches edit upstream files; overlay files are whole, so a patch to one would be overwritten.
      edited=$(sed -n 's|^+++ b/\([^\t]*\).*|\1|p' patches/*.patch | sort -u)
      if both=$(comm -12 <(echo "$edited") <(cut -d' ' -f2 <<<"$overlay" | sort) | grep .); then
          echo "a patch edits what a whole file replaces: $both"; exit 1
      fi
      for p in patches/*.patch; do patch -d $out/src -p1 -s --no-backup-if-mismatch -r - < "$p"; done
      while read -r from at; do install -D -m644 "$from" "$out/src/$at"; done <<<"$overlay"
      mkdir -p $out/src/firmware
      ${lib.optionalString (firmware != null) "install -m644 ${firmware}/* $out/src/firmware/"}
      # cfg80211 loads the signed regulatory database at boot, before any filesystem holds it.
      install -m644 ${pkgs.wireless-regdb}/lib/firmware/regulatory.db{,.p7s} $out/src/firmware/
    '';
  };

  # Placeholder for the defconfig's INITRAMFS_SOURCE: only the initial console.
  placeholder = pkgs.writeText "initramfs-placeholder.list" ''
    dir /dev 755 0 0
    nod /dev/console 600 0 0 c 5 1
  '';

  build = kernelDrv {
    name = "linux-${board}-build";
    buildCommand = ''
      ${kernelEnv}
      W=$NIX_BUILD_TOP/w
      mkdir -p "$O" $out
      cp ${placeholder} "$O/initramfs"
      m ${board}_defconfig
      # NULL_TTY: console=ttynull keeps /dev/console (systemd's emergency and panic-on-fail units) with no UART.
      for symbol in AIO UNIX98_PTYS USB_CONFIGFS_F_FS HW_RANDOM AX620E_CHIPKEY NULL_TTY; do
          grep -qx "CONFIG_''${symbol}=y" "$O/.config" || { echo "missing CONFIG_''${symbol}=y"; exit 1; }
      done
      m savedefconfig
      diff -u ${src}/src/arch/${kbuild}/configs/${board}_defconfig "$O/defconfig" ||
          { echo "${board}/arm64.defconfig: not what savedefconfig makes of it (+ above)"; exit 1; }
      ${lib.optionalString (firmware == null) ''
        bash "$S/scripts/config" --file "$O/.config" --set-str EXTRA_FIRMWARE "regulatory.db regulatory.db.p7s"
        m olddefconfig
      ''}
      # Fragments are merged after savedefconfig; each symbol must survive the merge (one with unmet dependencies
      # is silently dropped). CMDLINE is the exception: each fragment adds its words to it rather than replacing
      # the last one's.
      cmdline=
      for c in ${
        lib.concatMapStringsSep " " (f: "$S/arch/${kbuild}/configs/${board}-${lib.escapeShellArg f}") (
          [ "nixos.config" ] ++ fragments
        )
      } ${usbIds}; do
          # What `make <fragment>.config` runs, which finds only fragments in the source tree.
          "$S/scripts/kconfig/merge_config.sh" -m -O "$O" "$O/.config" "$c"
          m olddefconfig
          words=$(sed -n 's/^CONFIG_CMDLINE="\(.*\)"$/\1/p' "$c")
          cmdline="$cmdline''${words:+ $words}"
          if grep '^CONFIG_' "$c" | grep -v '^CONFIG_CMDLINE=' | grep -vxF -f "$O/.config"; then
              echo "$c: not in the merged .config (above)"; exit 1
          fi
      done
      if [ -n "$cmdline" ]; then
          cmdline="CONFIG_CMDLINE=\"''${cmdline# }\""
          sed -i "s|^CONFIG_CMDLINE=.*|$cmdline|" "$O/.config"
          m olddefconfig
          grep -qxF "$cmdline" "$O/.config" || { echo "not in the merged .config: $cmdline"; exit 1; }
      fi
      # USB is device-only: the controller in gadget mode, no host stack, no network function (each selects
      # U_ETHER), no precomposed gadget (each sets its own IDs, outside configfs; raw-gadget and gadgetfs let
      # userspace define its own interfaces outside FunctionFS), and no HID function unless usb-hid.config.
      grep -qx CONFIG_USB_DWC3_GADGET=y "$O/.config" || { echo "missing CONFIG_USB_DWC3_GADGET=y"; exit 1; }
      legacy=$(sed -n 's/^config //p' "$S/drivers/usb/gadget/legacy/Kconfig")
      for symbol in USB USB_U_ETHER $legacy ${
        lib.optionalString (!lib.elem "usb-hid.config" fragments) "USB_F_HID"
      }; do
          if grep "^CONFIG_''${symbol}=" "$O/.config"; then echo "must not be set: CONFIG_''${symbol}"; exit 1; fi
      done
      # Root cannot change the running kernel: firmware read only from the DT's verified root (LoadPin with
      # dm-verity), lockdown (integrity) unless driver-modules.config, the card write lock unless
      # no-boot-lock.config, and no kexec, hibernation, /dev/mem, /dev/port, /proc/kcore, DT overlays, userspace
      # drivers or MMC test driver.
      for symbol in SECURITY_LOADPIN_ENFORCE SECURITY_LOADPIN_VERITY ${
        lib.optionalString (
          !lib.elem "driver-modules.config" fragments
        ) "SECURITY_LOCKDOWN_LSM LOCK_DOWN_KERNEL_FORCE_INTEGRITY"
      } ${lib.optionalString (!lib.elem "no-boot-lock.config" fragments) "MMC_BLOCK_WRITE_LOCK"}; do
          grep -qx "CONFIG_''${symbol}=y" "$O/.config" || { echo "missing CONFIG_''${symbol}=y"; exit 1; }
      done
      for symbol in KEXEC KEXEC_FILE HIBERNATION DEVMEM DEVPORT PROC_KCORE OF_OVERLAY UIO VFIO MMC_TEST; do
          if grep "^CONFIG_''${symbol}=" "$O/.config"; then echo "must not be set: CONFIG_''${symbol}"; exit 1; fi
      done
      # No console on the UART unless serial-console.config, and never SysRq from it; the built-in command line
      # never names it.
      for symbol in MAGIC_SYSRQ_SERIAL ${
        lib.optionalString (!lib.elem "serial-console.config" fragments) "SERIAL_8250_CONSOLE"
      }; do
          if grep "^CONFIG_''${symbol}=" "$O/.config"; then echo "must not be set: CONFIG_''${symbol}"; exit 1; fi
      done
      if grep -E '^CONFIG_CMDLINE=.*(console=ttyS|earlycon=uart)' "$O/.config"; then
          echo "the built-in command line names the UART"; exit 1
      fi
      # Our sources and the DTB build with W=1 without warnings, in a separate output dir so the Image's objects
      # keep their flags.
      w() { k O="$W" W=1 "$@"; }
      w ${board}_defconfig
      w KCFLAGS=-Werror $dirs ${dtb} 2>&1 | tee $out/w1.log
      if grep -qi warning $out/w1.log; then echo "W=1 warnings (above)"; exit 1; fi
      # Vendor sources keep their own warning report.
      w drivers/net/wireless/aic8800/ 2>&1 | tee $out/vendor-w1.log
      m Image ${dtb}
      cp "$O/arch/${kbuild}/boot/dts/${dtb}" $out/
      # Loadable modules (driver-modules.config), installed without the links back to this build's trees.
      if grep -qx CONFIG_MODULES=y "$O/.config"; then
          m modules
          m INSTALL_MOD_PATH=$NIX_BUILD_TOP/mods modules_install
          mv $NIX_BUILD_TOP/mods/lib/modules/* $out/modules
          rm -f $out/modules/build $out/modules/source
      fi
      cp -a "$O" $out/o
    '';
  };

  # Our own C (not the vendor aic8800 tree): sparse over the kernel drivers, and checkpatch --strict over them and
  # over the C under `boot`, a source tree of boot-stage code. Findings are reported to $out, never fatal.
  lint =
    boot:
    kernelDrv {
      name = "linux-${board}-lint";
      nativeBuildInputs = [
        aarch64.bare
        pkgs.bc
        pkgs.bison
        pkgs.flex
        pkgs.openssl
        pkgs.perl
        pkgs.python3
        pkgs.sparse
      ];
      inherit boot;
      buildCommand = ''
        ${kernelEnv}
        mkdir -p "$O" $out
        m ${board}_defconfig
        m modules_prepare
        (cd ${./tree} && find . -path ./drivers/net/wireless/aic8800 -prune -o \( -name '*.c' -o -name '*.h' \) -print | sed 's|^\./||' | sort) > $NIX_BUILD_TOP/kfiles
        # Sparse on our objects only (-k: sources the defconfig leaves out have no rule).
        m -k C=1 CHECK=sparse $(sed -n 's/\.c$/.o/p' $NIX_BUILD_TOP/kfiles) > $NIX_BUILD_TOP/sparse.raw 2>&1 || true
        # Only findings in our files: prerequisites the make rebuilds are upstream's.
        grep -F -f $NIX_BUILD_TOP/kfiles $NIX_BUILD_TOP/sparse.raw | grep 'warning:\|error:' > $out/sparse.log || true
        # checkpatch: the generated tables (logo, pinmux) are data, not code.
        ck="perl $S/scripts/checkpatch.pl --root=$S --no-tree --strict --terse --show-types --no-summary --ignore UNDOCUMENTED_DT_STRING -f"
        (cd $S && xargs $ck < $NIX_BUILD_TOP/kfiles) > $out/checkpatch-kernel.log 2>&1 || true
        (cd $boot && find . \( -name '*.c' -o -name '*.h' \) ! -name 'logo_gram.h' ! -name '*pinmux.h' | sort | xargs $ck) > $out/checkpatch-boot.log 2>&1 || true
        for l in sparse checkpatch-kernel checkpatch-boot; do echo "$l: $(grep -c 'warning:\|error:\|WARNING:\|ERROR:\|CHECK:' $out/$l.log || true)"; done | tee $out/summary
        for l in checkpatch-kernel checkpatch-boot; do cut -d: -f1 $out/$l.log | sort | uniq -c | sort -rn > $out/$l.per-file; done
      '';
    };

  # build's output dir with the real initramfs: its objects all have mtime 1, like their sources, so make rebuilds
  # only the initramfs and what links it. `initramfs` is a derivation with a gen_init_cpio list in initramfs.list,
  # or a cpio file; a plain cpio is compressed per the defconfig, a compressed one is used as is (Kbuild's
  # *.cpio.* input).
  image =
    initramfs:
    kernelDrv (
      # No store path in the Image. A NixOS initrd's own store paths would read as references, so nixosKernel
      # checks that case on the same objects.
      lib.optionalAttrs (lib.isDerivation initramfs) { allowedReferences = [ ]; }
      // {
        name = "linux-${board}";
        buildCommand = ''
          ${kernelEnv}
          cp -a ${build}/o "$O" && chmod -R u+w "$O"
          i=${initramfs}
          if [ -d "$i" ]; then f=initramfs i=$i/initramfs.list
          elif [ "$(head -c 6 "$i")" = 070701 ]; then f=initramfs.cpio
          else f=initramfs.cpio.packed
          fi
          if [ $f = initramfs.cpio.packed ]; then
              # The kernel opens /dev/console for init before devtmpfs is mounted; a NixOS initrd has none (no
              # mknod in the build), so a cpio with it goes first, as in the kernel's default initramfs.
              {
                  printf 'dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\n' | "$O/usr/gen_init_cpio" -t 1 -
                  cat "$i"
              } > "$O/$f"
          else
              cp "$i" "$O/$f"
          fi
          # All mtimes are 1, so make cannot see a changed list; remove the old cpio.
          rm "$O/usr/initramfs_data.cpio"
          touch $NIX_BUILD_TOP/before
          m CONFIG_INITRAMFS_SOURCE=$f Image
          # Fail if any object other than the initramfs and vmlinux was rebuilt.
          stray=$(cd "$O" && find . -newer $NIX_BUILD_TOP/before -name '*.o' \
              ! -path ./usr/initramfs_data.o ! -path ./vmlinux.o ! -name '.tmp_vmlinux*')
          [ -z "$stray" ] || { echo "recompiled, not just relinked:" $stray; exit 1; }
          mkdir $out
          cp "$O/arch/${kbuild}/boot/Image" ${build}/${baseNameOf dtb} $out/
        '';
      }
    );

  # build as NixOS's boot.kernelPackages without a second compile: version, modDirVersion, the config, dtbs, and a
  # modules tree: modules.builtin (so the initrd's module closure finds erofs, overlay, loop and autofs built in) and
  # any loadable modules. An empty Image: the system boots `image <NixOS initrd>`; the toplevel only links one. The
  # config is parsed from the committed defconfig and fragments, so evaluation never builds the kernel; symbols
  # savedefconfig leaves at their defaults read as unset, which only turns off optional NixOS features (with
  # `features` set, NixOS checks no kernel config).
  nixosKernel =
    let
      o = "${build}/o";
      version = lib.removeSuffix ".tar.xz" (lib.removePrefix "linux-" upstream.linux-src.name);
      modDirVersion = version;
      # A fragment's line replaces the defconfig's, as in the merge.
      parse =
        file:
        lib.listToAttrs (
          lib.concatMap (
            line:
            let
              set = lib.match "(CONFIG_[^=]+)=([ym])" line;
              unset = lib.match "# (CONFIG_[^ ]+) is not set" line;
            in
            lib.optional (set != null) (lib.nameValuePair (lib.elemAt set 0) (lib.elemAt set 1))
            ++ lib.optional (unset != null) (lib.nameValuePair (lib.head unset) "n")
          ) (lib.splitString "\n" (builtins.readFile (boardDir + "/${file}")))
        );
      parsed = lib.foldl' (acc: file: acc // parse file) { } (
        [
          "arm64.defconfig"
          "nixos.config"
        ]
        ++ fragments
      );
      get = attr: parsed."CONFIG_${attr}" or null;
      config = parsed // rec {
        isSet = attr: parsed ? "CONFIG_${attr}";
        getValue = get;
        isYes = attr: get attr == "y";
        isNo = attr: get attr == "n";
        isModule = attr: get attr == "m";
        isEnabled = attr: isYes attr || isModule attr;
        isDisabled = attr: !isSet attr || isNo attr;
      };
    in
    # Arguments NixOS overrides; patches and a randstruct seed would need a separate compile.
    lib.makeOverridable (
      {
        features ? { },
        kernelPatches ? [ ],
        randstructSeed ? "",
      }:
      assert kernelPatches == [ ] && randstructSeed == "";
      pkgs.runCommand "linux-${board}-${version}"
        {
          # No store path in what NixOS boots or references.
          allowedReferences = [ ];
          passthru = {
            inherit
              version
              modDirVersion
              config
              features
              kernelPatches
              ;
            inherit (devicePkgs) stdenv;
            # The final merged .config.
            configfile = "${o}/.config";
            isZen = false;
            kernelOlder = lib.versionOlder version;
            kernelAtLeast = lib.versionAtLeast version;
          };
        }
        ''
          [ "$(cat ${o}/include/config/kernel.release)" = ${modDirVersion} ] ||
            { echo "kernel.release is not ${modDirVersion} (a LOCALVERSION?)"; exit 1; }
          mkdir -p $out/lib/modules/${modDirVersion} $out/dtbs/axera
          touch $out/Image
          # No store path in the kernel the system's Image is built from.
          ! grep -q "$NIX_STORE/[a-z0-9]\{32\}-" ${o}/arch/${kbuild}/boot/Image ||
            { echo "the Image holds a store path"; exit 1; }
          cp ${build}/${baseNameOf dtb} $out/dtbs/axera/
          cp ${o}/modules.builtin ${o}/modules.builtin.modinfo $out/lib/modules/${modDirVersion}/
          if [ -d ${build}/modules ]; then
            cp -r ${build}/modules/kernel ${build}/modules/modules.order $out/lib/modules/${modDirVersion}/
          else
            # No CONFIG_MODULES: an empty list, which NixOS's initrd copies.
            touch $out/lib/modules/${modDirVersion}/modules.order
          fi
          # The maps modprobe reads (built-in aliases such as autofs → autofs4), as modules_install makes them;
          # NixOS links a lone kernel's module tree as is, without depmod.
          ${pkgs.kmod}/bin/depmod -b $out -a ${modDirVersion}
        ''
    ) { };
in
{
  inherit
    src
    build
    lint
    image
    ;
  nixosPackages = devicePkgs.linuxPackagesFor nixosKernel;
}
