/* The card layout, slot tries, default env and bundle header U-Boot shares with the OS. Generated; do not edit. */
#ifndef BOOTCHAIN_H
#define BOOTCHAIN_H
#define SLOT_A_LBA 286720
#define SLOT_B_LBA 2383872
#define SLOT_FIT_AT 1964032
#define SLOT_FIT_MAX 41943040
#define STAGED_AT 2045952
#define RECOVERY_LBA 4481024
#define RECOVERY_FIT_MAX 26214400
#define FW_A_LBA 264192
#define FW_B_LBA 272384
#define FW_MAX 3145728
#define BOOT_BIN_LBA 19904
#define STOCK_P1_TYPE 0xc
#define STOCK_P1_LBA 2048
#define STOCK_P1_SECTORS 262144
#define STOCK_P2_LBA 264192
#define TRIES_CONFIRMED 3
#define POWER_ONS_PER_TRY 4
#define AXERA_MAGIC 0x55543322
#define AXERA_HDR 1024
#define AXERA_RSA_3072 0x400
#define AXERA_CIPHER 0x100
#define FW_KEY_AT 44
#define FW_KEY_LEN 396
#define FW_TABLE_LEN 56
#define BOOTCHAIN_DEFAULT_ENV "BOOT_ORDER=A B\0" "BOOT_A_LEFT=2\0" "BOOT_B_LEFT=2\0"
#endif
