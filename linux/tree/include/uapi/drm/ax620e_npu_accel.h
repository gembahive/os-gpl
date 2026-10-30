/* SPDX-License-Identifier: MIT */
/*
 * The AX620E NPU as a DRM accel device (drivers/accel/ax620e-npu).
 *
 * Userspace names a model, makes one buffer per tensor of at least the size
 * the kernel reports, and submits {model, buffers}. The kernel builds the
 * command stream from a model file in /lib/firmware. SUBMIT returns when the
 * job is done.
 *
 * Every struct has a fixed size and explicit padding, so the layout is the
 * same for 32- and 64-bit processes.
 */
#ifndef __DRM_UAPI_AX620E_NPU_ACCEL_H__
#define __DRM_UAPI_AX620E_NPU_ACCEL_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_AX620E_NPU_LOAD_MODEL		0x00
#define DRM_AX620E_NPU_CREATE_BO		0x01
#define DRM_AX620E_NPU_SUBMIT			0x02

#define DRM_IOCTL_AX620E_NPU_LOAD_MODEL	DRM_IOWR(DRM_COMMAND_BASE + DRM_AX620E_NPU_LOAD_MODEL, struct drm_ax620e_npu_load_model)
#define DRM_IOCTL_AX620E_NPU_CREATE_BO	DRM_IOWR(DRM_COMMAND_BASE + DRM_AX620E_NPU_CREATE_BO, struct drm_ax620e_npu_create_bo)
#define DRM_IOCTL_AX620E_NPU_SUBMIT	DRM_IOW(DRM_COMMAND_BASE + DRM_AX620E_NPU_SUBMIT, struct drm_ax620e_npu_submit)

/* Model and tensor names: NUL-terminated within this many bytes. */
#define DRM_AX620E_NPU_NAME_LEN		32
/* Tensors per model the caller sees: inputs, then outputs. */
#define DRM_AX620E_NPU_MAX_TENSORS	8

/**
 * struct drm_ax620e_npu_load_model - load /lib/firmware/npu/<name>.{neu,params}
 *
 * The model stays loaded until the file is closed. -ENODEV once the NPU is
 * dead (see SUBMIT).
 */
struct drm_ax620e_npu_load_model {
	/** @name: in, [a-z0-9_], NUL-terminated. */
	char name[DRM_AX620E_NPU_NAME_LEN];
	/** @handle: out, for SUBMIT. */
	__u32 handle;
	/** @n_inputs: out. */
	__u32 n_inputs;
	/** @n_outputs: out. */
	__u32 n_outputs;
	/** @pad: must be zero. */
	__u32 pad;
	/** @sizes: out, each tensor's size in bytes: inputs, then outputs. */
	__u32 sizes[DRM_AX620E_NPU_MAX_TENSORS];
	/** @names: out, each tensor's name, in the order of @sizes. */
	char names[DRM_AX620E_NPU_MAX_TENSORS][DRM_AX620E_NPU_NAME_LEN];
};

/**
 * struct drm_ax620e_npu_create_bo - a contiguous, write-combined buffer
 *
 * Map it with mmap() on the device file at @offset. Buffers also cross
 * drivers as dma-bufs (DRM_IOCTL_PRIME_*); an imported one must be
 * contiguous.
 */
struct drm_ax620e_npu_create_bo {
	/** @size: in, bytes. */
	__u32 size;
	/** @handle: out, a GEM handle. */
	__u32 handle;
	/** @offset: out, the mmap offset. */
	__u64 offset;
};

/**
 * struct drm_ax620e_npu_submit - run a loaded model once, synchronously
 *
 * Returns 0 when the NPU signals the end of the job and has stopped; -EIO on
 * an NPU error and -ETIMEDOUT if it never signals, after the kernel has
 * reset the NPU. If that reset fails, -EIO, and the NPU is dead: every later
 * SUBMIT and LOAD_MODEL returns -ENODEV. Also -EINVAL for a buffer smaller
 * than its tensor or not 32-byte aligned (an imported dma-buf can be),
 * -ENOENT for an unknown model or buffer handle, and -EINTR if a signal
 * arrives while waiting for another job (a running job is never interrupted).
 */
struct drm_ax620e_npu_submit {
	/** @model: in, a LOAD_MODEL handle. */
	__u32 model;
	/** @n_bos: in, must equal the model's n_inputs + n_outputs. */
	__u32 n_bos;
	/** @bos: in, GEM handles in LOAD_MODEL's tensor order, each at least that size. */
	__u32 bos[DRM_AX620E_NPU_MAX_TENSORS];
};

#if defined(__cplusplus)
}
#endif

#endif /* __DRM_UAPI_AX620E_NPU_ACCEL_H__ */
