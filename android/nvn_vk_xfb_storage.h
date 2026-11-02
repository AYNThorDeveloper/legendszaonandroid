#pragma once
/* Lower simple SPIR-V XFB outputs to bounded storage-buffer writes, as used
 * by PokeZ-ARecomp. Pure transformation: independently validated on the host.
 * The cached module stays unchanged; unsupported output layouts keep native XFB. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t mask, binding[4], stride[4]; } NvkXfbStorage;
typedef struct { uint32_t *w; size_t n, cap; int failed; } XfbWords;
static void xfb_append(XfbWords *s, const uint32_t *w, size_t n) {
    if (s->failed) return;
    if (s->n + n > s->cap) {
        size_t cap = (s->n + n) * 2 + 256;
        uint32_t *p = realloc(s->w, cap * sizeof *p);
        if (!p) { s->failed = 1; return; }
        s->w = p; s->cap = cap;
    }
    memcpy(s->w + s->n, w, n * sizeof *w); s->n += n;
}
static void xfb_instruction(XfbWords *s, unsigned op, const uint32_t *args, size_t n) {
    uint32_t first = (uint32_t)(n + 1) << 16 | op;
    xfb_append(s, &first, 1); xfb_append(s, args, n);
}
#define XFB_I(s, op, ...) xfb_instruction((s), (op), (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))
typedef struct { uint32_t op, a, b, buf, stride, offset; } XfbNode;
typedef struct { uint32_t var, type, scalar, components, buffer, offset, constant_offset, constant_last; } XfbOutput;

/* Push constants: word base[4], word limit[4], first vertex, session cursor.
 * Descriptor offsets are aligned; word bases retain the exact NVN addresses.
 * limit is exclusive and excludes alignment padding after the bound range. */
static uint32_t *nvk_spv_xfb_storage(const uint32_t *w, size_t nw, uint32_t first_binding,
                                    size_t *out_nw, NvkXfbStorage *layout) {
    if (nw < 5 || w[0] != 0x07230203 || w[1] < 0x00010300 || !w[3] || w[3] > (1u << 20)) return NULL;
    uint32_t bound = w[3], next = bound, entry = 0, uint_type = 0, int_type = 0, bool_type = 0, vertex = 0;
    XfbNode *nodes = calloc(bound, sizeof *nodes);
    if (!nodes) return NULL;
    for (uint32_t i = 0; i < bound; ++i) nodes[i].buf = UINT32_MAX;
    XfbOutput outputs[32]; unsigned nout = 0;
    XfbWords anno = {0}, globals = {0}, result = {0};
    NvkXfbStorage meta = {0};
    for (size_t i = 5; i < nw;) {
        unsigned n = w[i] >> 16, op = w[i] & 65535;
        if (!n || n > nw - i) goto fail;
        const uint32_t *p = w + i;
        if (op == 15 && n >= 4 && p[1] == 0) { if (entry && entry != p[2]) goto fail; entry = p[2]; }
        if (op == 20 && n == 2 && p[1] < bound) { nodes[p[1]].op = op; bool_type = p[1]; }
        if ((op == 21 || op == 22 || op == 23 || op == 32) && n >= 3 && p[1] < bound) {
            nodes[p[1]].op = op; nodes[p[1]].a = p[2]; nodes[p[1]].b = n >= 4 ? p[3] : 0;
            if (op == 21 && p[2] == 32) { if (p[3]) int_type = p[1]; else uint_type = p[1]; }
        }
        if (op == 59 && n >= 4 && p[2] < bound) {
            nodes[p[2]].op = op; nodes[p[2]].a = p[1]; nodes[p[2]].b = p[3];
            if (p[3] == 9) goto fail; /* do not collide with an existing push-constant block */
        }
        if (op == 71 && n >= 4 && p[1] < bound) {
            XfbNode *v = &nodes[p[1]];
            if (p[2] == 36) v->buf = p[3];
            if (p[2] == 37) v->stride = p[3];
            if (p[2] == 35) v->offset = p[3];
            if (p[2] == 11 && p[3] == 42) vertex = p[1];
        }
        i += n;
    }
    if (!entry) goto fail;
    for (uint32_t id = 0; id < bound; ++id) {
        XfbNode v = nodes[id];
        if (v.buf == UINT32_MAX) continue;
        if (v.op != 59 || v.b != 3 || v.buf >= 4 || !v.stride || (v.stride & 3) || (v.offset & 3) || nout == 32 || v.a >= bound) goto fail;
        XfbNode ptr = nodes[v.a];
        if (ptr.op != 32 || ptr.a != 3 || ptr.b >= bound) goto fail;
        uint32_t type = ptr.b, scalar = type, count = 1;
        if (nodes[type].op == 23) { scalar = nodes[type].a; count = nodes[type].b; }
        if (scalar >= bound || nodes[scalar].op != 22 || nodes[scalar].a != 32 || !count || count > 4 || (uint64_t)v.offset + count * 4 > v.stride) goto fail;
        if ((meta.mask & (1u << v.buf)) && meta.stride[v.buf] != v.stride) goto fail;
        meta.mask |= 1u << v.buf; meta.stride[v.buf] = v.stride;
        outputs[nout++] = (XfbOutput){.var = id, .type = type, .scalar = scalar, .components = count,
            .buffer = v.buf, .offset = v.offset / 4};
    }
    if (!nout) goto fail;
    if (!uint_type) { uint_type = next++; XFB_I(&globals, 21, uint_type, 32, 0); }
    if (!int_type) { int_type = next++; XFB_I(&globals, 21, int_type, 32, 1); }
    if (!bool_type) { bool_type = next++; XFB_I(&globals, 20, bool_type); }
    uint32_t vertex_type = int_type;
    int add_vertex = !vertex;
    if (add_vertex) {
        uint32_t ptr = next++; XFB_I(&globals, 32, ptr, 1, int_type);
        vertex = next++; XFB_I(&globals, 59, ptr, vertex, 1);
        XFB_I(&anno, 71, vertex, 11, 42);
    } else {
        XfbNode v = nodes[vertex];
        if (v.op != 59 || v.b != 1 || v.a >= bound || nodes[v.a].op != 32) goto fail;
        vertex_type = nodes[v.a].b;
        if (vertex_type >= bound || nodes[vertex_type].op != 21 || nodes[vertex_type].a != 32) goto fail;
    }
    uint32_t c[10];
    for (unsigned i = 0; i < 10; ++i) { c[i] = next++; XFB_I(&globals, 43, uint_type, c[i], i); }
    uint32_t array = next++, block = next++, block_ptr = next++, word_ptr = next++;
    XFB_I(&globals, 29, array, uint_type);
    XFB_I(&globals, 30, block, array);
    XFB_I(&globals, 32, block_ptr, 12, block);
    XFB_I(&globals, 32, word_ptr, 12, uint_type);
    XFB_I(&anno, 71, array, 6, 4);
    XFB_I(&anno, 71, block, 2);
    XFB_I(&anno, 72, block, 0, 35, 0);
    uint32_t buffers[4] = {0}, strides[4] = {0};
    for (unsigned b = 0; b < 4; ++b) if (meta.mask & (1u << b)) {
        buffers[b] = next++; XFB_I(&globals, 59, block_ptr, buffers[b], 12);
        meta.binding[b] = first_binding++;
        XFB_I(&anno, 71, buffers[b], 34, 0);
        XFB_I(&anno, 71, buffers[b], 33, meta.binding[b]);
        strides[b] = next++; XFB_I(&globals, 43, uint_type, strides[b], meta.stride[b] / 4);
    }
    uint32_t pc_type = next++, pc_ptr = next++, pc_word_ptr = next++, pc = next++;
    uint32_t members[11] = {pc_type}; for (unsigned i = 1; i < 11; ++i) members[i] = uint_type;
    xfb_instruction(&globals, 30, members, 11);
    XFB_I(&globals, 32, pc_ptr, 9, pc_type);
    XFB_I(&globals, 32, pc_word_ptr, 9, uint_type);
    XFB_I(&globals, 59, pc_ptr, pc, 9);
    XFB_I(&anno, 71, pc_type, 2);
    for (unsigned i = 0; i < 10; ++i) XFB_I(&anno, 72, pc_type, i, 35, i * 4);
    for (unsigned o = 0; o < nout; ++o) {
        outputs[o].constant_offset = next++; outputs[o].constant_last = next++;
        XFB_I(&globals, 43, uint_type, outputs[o].constant_offset, outputs[o].offset);
        XFB_I(&globals, 43, uint_type, outputs[o].constant_last, outputs[o].components - 1);
    }
    int annotations_added = 0, globals_added = 0, in_entry = 0;
    xfb_append(&result, w, 5);
    for (size_t i = 5; i < nw;) {
        unsigned n = w[i] >> 16, op = w[i] & 65535;
        const uint32_t *p = w + i;
        if (!annotations_added && op >= 19 && op <= 39) { xfb_append(&result, anno.w, anno.n); annotations_added = 1; }
        if (!globals_added && op == 54) { xfb_append(&result, globals.w, globals.n); globals_added = 1; }
        if (op == 54) in_entry = n >= 3 && p[2] == entry;
        if (op == 56) in_entry = 0;
        if ((op == 17 && n == 2 && p[1] == 53) || (op == 16 && n >= 3 && p[1] == entry && p[2] == 11)) { i += n; continue; }
        if (op == 71 && n >= 3 && p[1] < bound && nodes[p[1]].buf != UINT32_MAX && (p[2] == 35 || p[2] == 36 || p[2] == 37 || p[2] == 29)) { i += n; continue; }
        if (op == 15 && n >= 4 && p[1] == 0 && p[2] == entry) {
            size_t start = result.n; xfb_append(&result, p, n);
            if (add_vertex) xfb_append(&result, &vertex, 1);
            if (w[1] >= 0x00010400) {
                for (unsigned b = 0; b < 4; ++b) if (buffers[b]) xfb_append(&result, buffers + b, 1);
                xfb_append(&result, &pc, 1);
            }
            if (result.failed) goto fail;
            result.w[start] = (uint32_t)(result.n - start) << 16 | 15;
        } else {
            if (in_entry && op == 253) {
                uint32_t vid = next++, vu = next++, first_ptr = next++, first = next++, cursor_ptr = next++, cursor = next++, relative = next++, index = next++;
                XFB_I(&result, 61, vertex_type, vid, vertex);
                XFB_I(&result, vertex_type == uint_type ? 83 : 124, uint_type, vu, vid);
                XFB_I(&result, 65, pc_word_ptr, first_ptr, pc, c[8]); XFB_I(&result, 61, uint_type, first, first_ptr);
                XFB_I(&result, 65, pc_word_ptr, cursor_ptr, pc, c[9]); XFB_I(&result, 61, uint_type, cursor, cursor_ptr);
                XFB_I(&result, 130, uint_type, relative, vu, first); XFB_I(&result, 128, uint_type, index, relative, cursor);
                for (unsigned o = 0; o < nout; ++o) {
                    XfbOutput out = outputs[o]; unsigned b = out.buffer;
                    uint32_t base_ptr = next++, base = next++, lim_ptr = next++, lim = next++, mul = next++, begin = next++, off_id = out.constant_offset, at = next++, last_id = out.constant_last, last = next++, ok = next++, yes = next++, merge = next++;
                    XFB_I(&result, 65, pc_word_ptr, base_ptr, pc, c[b]); XFB_I(&result, 61, uint_type, base, base_ptr);
                    XFB_I(&result, 65, pc_word_ptr, lim_ptr, pc, c[4 + b]); XFB_I(&result, 61, uint_type, lim, lim_ptr);
                    XFB_I(&result, 132, uint_type, mul, index, strides[b]); XFB_I(&result, 128, uint_type, begin, base, mul);
                    XFB_I(&result, 128, uint_type, at, begin, off_id); XFB_I(&result, 128, uint_type, last, at, last_id);
                    XFB_I(&result, 176, bool_type, ok, last, lim);
                    XFB_I(&result, 247, merge, 0); XFB_I(&result, 250, ok, yes, merge); XFB_I(&result, 248, yes);
                    uint32_t value = next++; XFB_I(&result, 61, out.type, value, out.var);
                    for (unsigned comp = 0; comp < out.components; ++comp) {
                        uint32_t scalar = value;
                        if (out.components > 1) { scalar = next++; XFB_I(&result, 81, out.scalar, scalar, value, comp); }
                        uint32_t bits = next++, word = next++, ptr = next++;
                        XFB_I(&result, 124, uint_type, bits, scalar); XFB_I(&result, 128, uint_type, word, at, c[comp]);
                        XFB_I(&result, 65, word_ptr, ptr, buffers[b], c[0], word); XFB_I(&result, 62, ptr, bits);
                    }
                    XFB_I(&result, 249, merge); XFB_I(&result, 248, merge);
                }
            }
            xfb_append(&result, p, n);
        }
        i += n;
    }
    if (!annotations_added || !globals_added || result.failed || anno.failed || globals.failed) goto fail;
    result.w[3] = next; *out_nw = result.n; *layout = meta;
    free(nodes); free(anno.w); free(globals.w); return result.w;
fail:
    free(nodes); free(anno.w); free(globals.w); free(result.w); return NULL;
}
#undef XFB_I
