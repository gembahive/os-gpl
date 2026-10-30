# Fixed-output upstream sources.
{ fetchurl }:
{
  # Checksum: kernel.org sha256sums.asc.
  linux-src = fetchurl {
    url = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.57.tar.xz";
    sha256 = "5bd9acdc82ec5efc3790ae840e5aaa40dfde7efff261cf6dde902dc570acf98c";
  };

  # Tarball signature u-boot-<v>.tar.bz2.sig: Tom Rini, F3CEA8743D60E0192F9B4C7A2BE2A0F50ABFE40A.
  u-boot-src = fetchurl {
    url = "https://ftp.denx.de/pub/u-boot/u-boot-2026.07.tar.bz2";
    sha256 = "78e8bfc382fe388f9b55aa1daf8c563522a037779b5d4c349d1415e381f1243e";
  };

  # fw_printenv sources.
  libubootenv-src = fetchurl {
    url = "https://github.com/sbabic/libubootenv/archive/refs/tags/v0.3.7.tar.gz";
    name = "libubootenv-0.3.7.tar.gz";
    sha256 = "92021fc5a0a965030588d0c867a771d28b28c0d17c9fd022f0dafe9f01de714e";
  };
  zlib-src = fetchurl {
    url = "https://zlib.net/fossils/zlib-1.3.1.tar.gz";
    sha256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23";
  };

  # Ubuntu's source package of busybox-static 1.30.1-7ubuntu3.1.
  busybox-dsc = fetchurl {
    url = "https://launchpad.net/ubuntu/+archive/primary/+files/busybox_1.30.1-7ubuntu3.1.dsc";
    sha256 = "c1140501b5666fcf2c29f371016290dfc52964a4c303b352c565a8ce78ce7a95";
  };
  busybox-orig = fetchurl {
    url = "https://launchpad.net/ubuntu/+archive/primary/+files/busybox_1.30.1.orig.tar.bz2";
    sha256 = "3d1d04a4dbd34048f4794815a5c48ebb9eb53c5277e09ffffc060323b95dfbdc";
  };
  busybox-debian = fetchurl {
    url = "https://launchpad.net/ubuntu/+archive/primary/+files/busybox_1.30.1-7ubuntu3.1.debian.tar.xz";
    sha256 = "5bc1660e21fb5a0b2ada3e26ae4e2d20d50477555e56df72503f0f091c4fd653";
  };
}
