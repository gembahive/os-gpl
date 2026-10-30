<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="brand/gembahive-logo-for-dark-backgrounds.svg">
    <img alt="GembaHive" src="brand/gembahive-logo-for-light-backgrounds.svg" width="320">
  </picture>
</p>

<p align="center">GPL and LGPL sources of the GembaHive OS</p>

| Component | Licence | Build |
| --- | --- | --- |
| Linux 6.18.57 with our drivers, patches, configuration and device trees | GPL-2.0-only | `.#kernel` |
| U-Boot 2026.07, 32-bit and 64-bit | GPL-2.0-or-later | `.#uboot`, `.#uboot64` |
| fw_printenv and fw_setenv: libubootenv 0.3.7 with zlib 1.3.1's CRC-32 | LGPL-2.1-or-later; Zlib | `.#fw-env` |
| NixOS system with our builds of systemd, D-Bus, iproute2, nftables, chrony and e2fsprogs | systemd: LGPL-2.1-or-later; D-Bus: GPL-2.0-or-later OR AFL-2.1; iproute2, nftables, chrony, e2fsprogs: GPL-2.0 | `.#toplevel` |
| BusyBox 1.30.1, Ubuntu source package 1.30.1-7ubuntu3.1 | GPL-2.0-only | `.#busybox-dsc`, `.#busybox-orig`, `.#busybox-debian` |

Build on x86_64-linux with Nix: `nix build .#kernel`.

Bootloader stages other than U-Boot, firmware files and application software are not included.
