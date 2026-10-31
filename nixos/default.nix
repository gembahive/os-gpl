# The system's builds of systemd, D-Bus, nftables and chrony, and, as the module argument `gplPkgs`, those of
# iproute2 and e2fsprogs for the modules that install them.
{
  config,
  lib,
  pkgs,
  ...
}:
let
  gplPkgs = {
    # systemd without what nothing here uses: TPM2, FIDO2, home directories, machines and containers, portable
    # services, sysupdate, image imports, the journal's remote upload, the hardware database, the virtual console,
    # disk encryption and repart, kexec, password quality checks, archive support, core dumps (the kernel's
    # core_pattern discards them), BPF programs (no unit restricts file systems, interfaces or socket binds),
    # AppArmor (not in the kernel), the journal's sealing keys (gcrypt) and internationalised host names.
    # QR codes stay: the initrd lists systemd-bsod.
    systemd = pkgs.systemd.override {
      withTpm2Tss = false;
      withFido2 = false;
      withHomed = false;
      withMachined = false;
      withNspawn = false;
      withVmspawn = false;
      withPortabled = false;
      withSysupdate = false;
      withImportd = false;
      withRemote = false;
      withHwdb = false;
      withVConsole = false;
      withCryptsetup = false;
      withRepart = false;
      withKexectools = false;
      withPasswordQuality = false;
      withLibarchive = false;
      withCoredump = false;
      withLibBPF = false;
      withApparmor = false;
      withGcrypt = false;
      withLibidn2 = false;
      # The util-linux the system has anyway, not a second, minimal one for mount, fsck and sulogin.
      inherit (pkgs) util-linux;
    };

    # The bus links the system's systemd; by default dbus-broker links nixpkgs' full one and dbus the minimal one.
    # No X11 autolaunch.
    dbus = pkgs.dbus.override {
      systemdMinimal = config.systemd.package;
      x11Support = false;
    };
    dbus-broker = pkgs.dbus-broker.override {
      systemd = config.systemd.package;
      dbus = config.services.dbus.dbusPackage;
    };

    # No servers, so no NTS (GnuTLS).
    chrony = pkgs.chrony.overrideAttrs (o: {
      buildInputs = lib.filter (p: lib.getName p != "gnutls") o.buildInputs;
      configureFlags = o.configureFlags ++ [ "--disable-nts" ];
    });

    # Without tc's xtables actions, arpd (Berkeley DB) and BPF program loading (elfutils, libbpf).
    iproute2 = pkgs.iproute2.overrideAttrs (o: {
      buildInputs = lib.subtractLists [
        pkgs.db
        pkgs.iptables
        pkgs.elfutils
        pkgs.libbpf
      ] o.buildInputs;
    });

    # mke2fs without tarball import (-d), which links libarchive and libxml2.
    e2fsprogs = pkgs.e2fsprogs.overrideAttrs (o: {
      configureFlags = map (
        f: if f == "--with-libarchive=direct" then "--without-libarchive" else f
      ) o.configureFlags;
    });
  };
in
{
  _module.args = { inherit gplPkgs; };

  systemd.package = gplPkgs.systemd;
  systemd.coredump.enable = false;
  # NixOS links these tmpfiles whether or not systemd was built with portabled and nspawn; masked, as the links
  # dangle.
  environment.etc."tmpfiles.d/portables.conf".source = "/dev/null";
  environment.etc."tmpfiles.d/systemd-nspawn.conf".source = "/dev/null";
  # systemd without the hardware database builds none, and without the virtual console has no unit for it.
  environment.etc."udev/hwdb.bin".enable = false;
  console.enable = false;
  services.dbus.dbusPackage = gplPkgs.dbus;
  services.dbus.brokerPackage = gplPkgs.dbus-broker;
  services.chrony.package = gplPkgs.chrony;

  # The nftables module takes pkgs.nftables. `nft -f` loads the ruleset without its interactive mode or the
  # iptables libraries.
  nixpkgs.overlays = [
    (_: prev: {
      nftables = prev.nftables.override {
        withCli = false;
        withXtables = false;
      };
    })
  ];
}
