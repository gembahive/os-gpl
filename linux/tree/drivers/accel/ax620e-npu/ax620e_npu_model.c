// SPDX-License-Identifier: GPL-2.0-only
/*
 * The model's program file (<name>.neu): a FlatBuffers table, cut out of the
 * .axmodel at build time. Every offset, length and count is bounds-checked
 * against the file, and every OCM position against the OCM. The NPU itself
 * runs the command stream unchecked.
 *
 * Schema (field numbers are vtable slots):
 *
 *   root: f1 {f0 u8 soc}      soc must be 0
 *         f2 io
 *         f4 [{f0 [ubyte] data, f1 string name}]
 *         f5 {f1 [queue], f2 [8-byte words] command stream}
 *         f6 {f0 u32 ocm_usage, f1 u32 ocm_reserved, f2 [{f0 name, f1 u32 index}]}
 *         f9 u8 control_mode  must be 1
 *   io:   f0 [entry] inputs, f1 [entry] outputs, f2 [entry] ring buffer,
 *         f3 [entry] params copied to OCM
 *   entry: f0 name, f1 u64 offset (OCM offset for f3), f2 u64 size, f3 u8 in OCM
 *   queue: f0 u8 type, f1 u32 length, f2 u32 OCM start
 *
 * The f6 list maps names to runtime-variable (RTV) slots: 32-bit words at the
 * start of OCM that the kernel fills with buffer addresses before each run.
 * The input list also names "params", the weights (<name>.params), and one
 * 4-byte "<tensor>_offset" word per tensor, holding the low address bits
 * (phys & 0x1f) an RTV word cannot.
 */

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ax620e_npu.h"

#define NPU_MAX_IO	16
#define NPU_MAX_SPECS	64
#define NPU_MAX_WEIGHTS	16

struct fb {
	const u8 *buf;
	u64 len;
	int err;	/* sticky: once set, every read returns 0 */
};

/* A table: its position, its vtable's position and both sizes. */
struct fb_tab {
	u32 pos, vt, vsize, tsize;
};

/* A string in the file. */
struct fb_str {
	u32 pos, len;
};

static bool fb_in(struct fb *b, u64 pos, u64 n)
{
	if (b->err || pos > b->len || n > b->len - pos) {
		b->err = -EINVAL;
		return false;
	}
	return true;
}

static struct fb_tab fb_table(struct fb *b, u64 pos)
{
	struct fb_tab t = {};
	s64 vt;

	if (!fb_in(b, pos, 4))
		return t;
	vt = (s64)pos - (s32)get_unaligned_le32(b->buf + pos);
	if (vt < 0 || !fb_in(b, vt, 4))
		return t;
	t = (struct fb_tab){ pos, vt, get_unaligned_le16(b->buf + vt),
			     get_unaligned_le16(b->buf + vt + 2) };
	if (t.vsize < 4 || t.vsize & 1 || t.tsize < 4)
		b->err = -EINVAL;
	if (!fb_in(b, t.vt, t.vsize) || !fb_in(b, t.pos, t.tsize))
		t = (struct fb_tab){};
	return t;
}

/* A field's position, or 0 when absent; it must lie inside its table. */
static u32 fb_field(struct fb *b, const struct fb_tab *t, unsigned int f, u32 width)
{
	u32 off;

	if (b->err || 4 + 2 * f + 2 > t->vsize)
		return 0;
	off = get_unaligned_le16(b->buf + t->vt + 4 + 2 * f);
	if (off && (off < 4 || off + width > t->tsize)) {
		b->err = -EINVAL;
		return 0;
	}
	return off ? t->pos + off : 0;
}

static u8 fb_u8(struct fb *b, const struct fb_tab *t, unsigned int f)
{
	u32 p = fb_field(b, t, f, 1);

	return p ? b->buf[p] : 0;
}

static u32 fb_u32(struct fb *b, const struct fb_tab *t, unsigned int f)
{
	u32 p = fb_field(b, t, f, 4);

	return p ? get_unaligned_le32(b->buf + p) : 0;
}

/* A u64 field the driver only uses as a u32. */
static u32 fb_u64(struct fb *b, const struct fb_tab *t, unsigned int f)
{
	u32 p = fb_field(b, t, f, 8);
	u64 v = p ? get_unaligned_le64(b->buf + p) : 0;

	if (v > U32_MAX)
		b->err = -EINVAL;
	return v;
}

/* Follow an offset field to its target, which has at least a u32 there. */
static u32 fb_ref(struct fb *b, const struct fb_tab *t, unsigned int f)
{
	u32 p = fb_field(b, t, f, 4);
	u64 target;

	if (!p)
		return 0;
	target = (u64)p + get_unaligned_le32(b->buf + p);
	return fb_in(b, target, 4) ? target : 0;
}

static struct fb_tab fb_subtable(struct fb *b, const struct fb_tab *t, unsigned int f)
{
	u32 p = fb_ref(b, t, f);

	return p ? fb_table(b, p) : (struct fb_tab){};
}

/* A vector's element count (0 when absent); *start is its first element. */
static u32 fb_vector(struct fb *b, const struct fb_tab *t, unsigned int f, u32 elem,
		     u32 max, u32 *start)
{
	u32 p = fb_ref(b, t, f);
	u32 n;

	*start = 0;
	if (!p)
		return 0;
	n = get_unaligned_le32(b->buf + p);
	if (n > max)
		b->err = -EINVAL;
	if (!fb_in(b, (u64)p + 4, (u64)n * elem))
		return 0;
	*start = p + 4;
	return n;
}

/* Element i of a vector of tables. */
static struct fb_tab fb_element(struct fb *b, u32 start, u32 i)
{
	u32 p = start + 4 * i;

	return fb_table(b, (u64)p + get_unaligned_le32(b->buf + p));
}

/* A required string field, NUL-terminated in the file. */
static struct fb_str fb_string(struct fb *b, const struct fb_tab *t, unsigned int f)
{
	struct fb_str s = {};

	s.len = fb_vector(b, t, f, 1, U32_MAX - 4, &s.pos);
	if (!s.pos || !fb_in(b, s.pos, (u64)s.len + 1) || b->buf[s.pos + s.len]) {
		b->err = -EINVAL;
		return (struct fb_str){};
	}
	return s;
}

static bool fb_eq(const struct fb *b, struct fb_str s, const char *name, u32 len)
{
	return s.len == len && !memcmp(b->buf + s.pos, name, len);
}

static bool fb_streq(const struct fb *b, struct fb_str s, struct fb_str t)
{
	return fb_eq(b, s, (const char *)b->buf + t.pos, t.len);
}

/* The RTV slot named `name`; it must fall inside the OCM's reserved area. */
static u16 npu_rtv(struct fb *b, const struct fb_tab *rtv, const char *name, u32 len,
		   const struct npu_layout *l, bool optional)
{
	u32 specs, n = fb_vector(b, rtv, 2, 4, NPU_MAX_SPECS, &specs);

	for (u32 i = 0; i < n && !b->err; i++) {
		struct fb_tab spec = fb_element(b, specs, i);
		struct fb_str s = fb_string(b, &spec, 0);
		u32 index = fb_u32(b, &spec, 1);

		if (b->err || !fb_eq(b, s, name, len))
			continue;
		if (index < l->ocm_reserved / 4 && index < NPU_NO_RTV)
			return index;
		optional = false;
		break;
	}
	if (!optional)
		b->err = -EINVAL;
	return NPU_NO_RTV;
}

static u16 npu_rtv_of(struct fb *b, const struct fb_tab *rtv, struct fb_str name,
		      const struct npu_layout *l)
{
	return npu_rtv(b, rtv, (const char *)b->buf + name.pos, name.len, l, false);
}

struct npu_io {
	struct fb_str name;
	u32 size;
	bool output;
};

/* An input or output: in DDR, at the start of its buffer. */
static struct npu_io npu_io_entry(struct fb *b, u32 start, u32 i, bool output)
{
	struct fb_tab e = fb_element(b, start, i);
	struct npu_io io = { fb_string(b, &e, 0), fb_u64(b, &e, 2), output };

	if (fb_u64(b, &e, 1) || fb_u8(b, &e, 3) || !io.size)
		b->err = -EINVAL;
	return io;
}

/* For a 4-byte input "<t>_offset" where <t> is another entry: <t>'s index. */
static int npu_offset_target(const struct fb *b, const struct npu_io *io, u32 n_io, u32 i)
{
	static const char suffix[] = "_offset";
	const u32 slen = sizeof(suffix) - 1;
	struct fb_str t = io[i].name;

	if (io[i].output || io[i].size != 4 || t.len <= slen)
		return -1;
	t.len -= slen;
	if (memcmp(b->buf + t.pos + t.len, suffix, slen))
		return -1;
	for (u32 j = 0; j < n_io; j++)
		if (fb_streq(b, io[j].name, t))
			return j;
	return -1;
}

/*
 * Inputs and outputs: "params", the offset words and, in list order (inputs
 * first), the tensors the caller supplies.
 */
static void npu_parse_io(struct fb *b, const struct fb_tab *iot, const struct fb_tab *rtv,
			 struct npu_layout *l)
{
	struct npu_io io[NPU_MAX_IO];
	u32 in, out, n_in, n_out, n_io;

	n_in = fb_vector(b, iot, 0, 4, NPU_MAX_IO, &in);
	n_out = fb_vector(b, iot, 1, 4, NPU_MAX_IO - n_in, &out);
	n_io = n_in + n_out;
	for (u32 i = 0; i < n_io && !b->err; i++)
		io[i] = i < n_in ? npu_io_entry(b, in, i, false) :
				   npu_io_entry(b, out, i - n_in, true);

	for (u32 i = 0; i < n_io && !b->err; i++) {
		u16 rtv_index = npu_rtv_of(b, rtv, io[i].name, l);

		if (!io[i].output && fb_eq(b, io[i].name, "params", 6)) {
			l->params_size = io[i].size;
			l->params_rtv = rtv_index;
		} else if (npu_offset_target(b, io, n_io, i) >= 0) {
			if (l->n_slots == NPU_MAX_SLOTS) {
				b->err = -EINVAL;
				return;
			}
			l->slot_rtv[l->n_slots++] = rtv_index;
		} else {
			if (l->n_tensors == DRM_AX620E_NPU_MAX_TENSORS ||
			    io[i].name.len >= DRM_AX620E_NPU_NAME_LEN) {
				b->err = -EINVAL;
				return;
			}
			l->tensor[l->n_tensors++] = (struct npu_tensor){
				io[i].name.pos, io[i].name.len, io[i].size, rtv_index };
			l->n_inputs += !io[i].output;
		}
	}
}

/* The DDR ring buffer ("ddr_swap"): at most one. */
static void npu_parse_swap(struct fb *b, const struct fb_tab *iot, const struct fb_tab *rtv,
			   struct npu_layout *l)
{
	u32 start, n = fb_vector(b, iot, 2, 4, 1, &start);
	struct fb_tab e;

	if (!n)
		return;
	e = fb_element(b, start, 0);
	l->swap_size = fb_u64(b, &e, 2);
	if (l->swap_size)
		l->swap_rtv = npu_rtv_of(b, rtv, fb_string(b, &e, 0), l);
}

/*
 * Params kept in OCM: the bytes of the f4 weight of the same name, copied in
 * 32-bit words inside the reserved area. DDR params are not supported.
 */
static void npu_parse_blobs(struct fb *b, const struct fb_tab *root, const struct fb_tab *iot,
			    struct npu_layout *l)
{
	u32 params, weights, n_w, n = fb_vector(b, iot, 3, 4, NPU_MAX_BLOBS, &params);

	n_w = fb_vector(b, root, 4, 4, NPU_MAX_WEIGHTS, &weights);
	for (u32 i = 0; i < n && !b->err; i++) {
		struct fb_tab e = fb_element(b, params, i);
		struct fb_str name = fb_string(b, &e, 0);
		struct npu_blob *blob = &l->blob[l->n_blobs++];

		blob->ocm = fb_u64(b, &e, 1);
		blob->len = fb_u64(b, &e, 2);
		if (!fb_u8(b, &e, 3) || blob->ocm % 4 || blob->len % 4 ||
		    (u64)blob->ocm + blob->len > l->ocm_reserved)
			b->err = -EINVAL;
		blob->data = 0;
		for (u32 j = 0; j < n_w && !b->err && !blob->data; j++) {
			struct fb_tab w = fb_element(b, weights, j);
			u32 len = 0;

			if (fb_streq(b, fb_string(b, &w, 1), name))
				len = fb_vector(b, &w, 0, 1, U32_MAX - 4, &blob->data);
			if (blob->data && len != blob->len)
				b->err = -EINVAL;
		}
		if (!blob->data)
			b->err = -EINVAL;
	}
}

/* Queues and the command stream. */
static void npu_parse_mcode(struct fb *b, const struct fb_tab *root, u32 ocm_size,
			    struct npu_layout *l)
{
	struct fb_tab mcode = fb_subtable(b, root, 5);
	u32 start, n = fb_vector(b, &mcode, 1, 4, NPU_MAX_QUEUES, &start);
	u32 seen = 0;

	for (u32 i = 0; i < n && !b->err; i++) {
		struct fb_tab q = fb_element(b, start, i);
		struct npu_queue *nq = &l->queue[l->n_queues++];

		*nq = (struct npu_queue){ fb_u8(b, &q, 0), fb_u32(b, &q, 2), fb_u32(b, &q, 1) };
		if (nq->type > 1 || seen & BIT(nq->type) || !nq->len || nq->len % 32 ||
		    nq->start % 32 || (u64)nq->start + nq->len > ocm_size)
			b->err = -EINVAL;
		else
			seen |= BIT(nq->type);
	}
	l->cmdq_len = 8 * fb_vector(b, &mcode, 2, 8, U32_MAX / 8, &l->cmdq);
	if (!l->n_queues || !l->cmdq_len)
		b->err = -EINVAL;
}

static bool npu_blob_hits(const struct npu_layout *l, u16 rtv)
{
	for (u32 i = 0; rtv != NPU_NO_RTV && i < l->n_blobs; i++)
		if (4u * rtv + 4 > l->blob[i].ocm && 4u * rtv < l->blob[i].ocm + l->blob[i].len)
			return true;
	return false;
}

/*
 * The OCM params are copied after the RTV words: one that covered an RTV word
 * would replace a kernel-written address with file bytes, so such a model is
 * refused.
 */
static bool npu_blobs_hit_rtv(const struct npu_layout *l)
{
	bool hit = npu_blob_hits(l, l->params_rtv) || npu_blob_hits(l, l->swap_rtv) ||
		   npu_blob_hits(l, l->ocm_base_rtv);

	for (u32 i = 0; i < l->n_tensors; i++)
		hit |= npu_blob_hits(l, l->tensor[i].rtv);
	for (u32 i = 0; i < l->n_slots; i++)
		hit |= npu_blob_hits(l, l->slot_rtv[i]);
	return hit;
}

int npu_parse(const u8 *buf, size_t len, u32 ocm_size, struct npu_layout *l)
{
	struct fb b = { buf, len, 0 };
	struct fb_tab root, soc, iot, rtv;

	*l = (struct npu_layout){ .params_rtv = NPU_NO_RTV, .swap_rtv = NPU_NO_RTV };
	root = fb_table(&b, fb_in(&b, 0, 4) ? get_unaligned_le32(buf) : 0);
	soc = fb_subtable(&b, &root, 1);
	iot = fb_subtable(&b, &root, 2);
	rtv = fb_subtable(&b, &root, 6);
	if (b.err || !iot.pos || !rtv.pos || fb_u8(&b, &soc, 0) != 0 || fb_u8(&b, &root, 9) != 1)
		return -EINVAL;

	l->ocm_usage = fb_u32(&b, &rtv, 0);
	l->ocm_reserved = fb_u32(&b, &rtv, 1);
	if ((u64)l->ocm_usage + l->ocm_reserved > ocm_size)
		return -EINVAL;
	npu_parse_io(&b, &iot, &rtv, l);
	npu_parse_swap(&b, &iot, &rtv, l);
	npu_parse_blobs(&b, &root, &iot, l);
	npu_parse_mcode(&b, &root, ocm_size, l);
	l->ocm_base_rtv = npu_rtv(&b, &rtv, "_ocm_base", 9, l, true);
	if (b.err || l->n_tensors <= l->n_inputs || npu_blobs_hit_rtv(l))
		return -EINVAL;
	return 0;
}
