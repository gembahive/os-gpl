# libubootenv's fw_printenv, static, with only zlib's CRC32, and fw_setenv linked to it; NO_YAML_SUPPORT selects the
# classic fw_env.config.
{
  pkgs,
  devicePkgs,
  upstream,
  aarch64,
}:
pkgs.runCommand "fw-env"
  {
    nativeBuildInputs = [
      aarch64.cc
      pkgs.file
    ];
  }
  ''
    tar -xf ${upstream.libubootenv-src}
    tar -xf ${upstream.zlib-src}
    export SOURCE_DATE_EPOCH=0
    p=${aarch64.cc.targetPrefix}
    ''${p}cc -std=gnu99 -Os -static -DNDEBUG -DNO_YAML_SUPPORT -DVERSION='"0.3.7"' \
      -L${devicePkgs.glibc.static}/lib -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -Ilibubootenv-0.3.7/src -Izlib-1.3.1 \
      libubootenv-0.3.7/src/fw_printenv.c libubootenv-0.3.7/src/uboot_env.c \
      libubootenv-0.3.7/src/uboot_mtd.c libubootenv-0.3.7/src/common.c \
      libubootenv-0.3.7/src/extended_config.c zlib-1.3.1/crc32.c -o fw_printenv
    ''${p}strip fw_printenv
    # One binary; it behaves as fw_setenv when run under that name.
    ln -s fw_printenv fw_setenv
    T=$PWD/env-test
    mkdir $T
    truncate -s 65536 $T/env0 $T/env1
    printf '%s 0x0 0x10000\n' $T/env0 $T/env1 > $T/config
    printf 'BOOT_ORDER=A B\nBOOT_A_LEFT=3\nBOOT_B_LEFT=0\n' > $T/default
    printf 'BOOT_ORDER=B A\nBOOT_A_LEFT=3\nBOOT_B_LEFT=1\n' | ./fw_setenv -c $T/config -f $T/default -s -
    test "$(./fw_printenv -c $T/config -n BOOT_ORDER)" = 'B A'
    test "$(./fw_printenv -c $T/config -n BOOT_B_LEFT)" = 1
    printf 'BOOT_ORDER=A B\nBOOT_B_LEFT=0\n' | ./fw_setenv -c $T/config -s -
    test "$(./fw_printenv -c $T/config -n BOOT_ORDER)" = 'A B'
    test "$(./fw_printenv -c $T/config -n BOOT_B_LEFT)" = 0
    mkdir $out
    cp -P fw_printenv fw_setenv $out/
    # A static aarch64 executable.
    file -L $out/fw_printenv | grep "ARM aarch64" | grep -q "statically linked" || { file -L $out/fw_printenv; exit 1; }
  ''
