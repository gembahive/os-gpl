/* SPDX-License-Identifier: GPL-2.0+ */
/* The EIP-130's signed firmware image. */

.section .rodata
.balign 4
.global ce_fw
ce_fw:
.incbin CE_FW
.global ce_fw_end
ce_fw_end:
