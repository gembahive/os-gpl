/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * What the driver takes from a model's program file (<name>.neu): the parse
 * result is positions and sizes in that file, valid while the file is held.
 */
#ifndef __AX620E_NPU_H__
#define __AX620E_NPU_H__

#include <linux/types.h>
#include <drm/ax620e_npu_accel.h>

#define NPU_MAX_QUEUES		2	/* IO and COMPUTE */
#define NPU_MAX_SLOTS		DRM_AX620E_NPU_MAX_TENSORS
#define NPU_MAX_BLOBS		4
#define NPU_NO_RTV		0xffff

/* A tensor the caller supplies a buffer for. */
struct npu_tensor {
	u32 name;		/* string position in the file */
	u32 name_len;
	u32 size;
	u16 rtv;
};

/* Bytes the kernel copies into OCM before every run. */
struct npu_blob {
	u32 data;		/* position in the file */
	u32 len;
	u32 ocm;		/* OCM offset */
};

struct npu_queue {
	u8 type;		/* 0 IO, 1 COMPUTE */
	u32 start;		/* OCM offset */
	u32 len;		/* bytes */
};

struct npu_layout {
	u32 ocm_usage, ocm_reserved;
	u32 cmdq, cmdq_len;	/* command stream: position and bytes */
	u32 n_queues;
	struct npu_queue queue[NPU_MAX_QUEUES];
	u32 n_inputs, n_tensors;	/* inputs first */
	struct npu_tensor tensor[DRM_AX620E_NPU_MAX_TENSORS];
	/* RTV slots of the "<tensor>_offset" words added to each tensor's address; always 0 as buffers are 32-byte aligned. */
	u32 n_slots;
	u16 slot_rtv[NPU_MAX_SLOTS];
	u32 n_blobs;
	struct npu_blob blob[NPU_MAX_BLOBS];
	u32 params_size, swap_size;	/* swap_size 0: none */
	u16 params_rtv, swap_rtv, ocm_base_rtv;
};

int npu_parse(const u8 *buf, size_t len, u32 ocm_size, struct npu_layout *l);

#endif
