/* Checked final-target population. No source or destination access before all spans pass. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "nv_iop_pixel.h"

enum nv_frame_result { NV_FRAME_OK, NV_FRAME_ARGUMENT, NV_FRAME_FORMAT, NV_FRAME_BOUNDS };
struct nv_frame_span {
    const void *address;
    uint64_t capacity;
    uint32_t width, height, pitch, format;
};
static inline int nv_frame_validate(const struct nv_frame_span *s)
{
    uint64_t row, full, visible;
    if (!s || !s->address || !s->width || !s->height || !s->pitch ||
        ((uintptr_t)s->address & 3u) || (s->pitch & 3u)) return NV_FRAME_ARGUMENT;
    if (s->format != NV_IOP_FMT_BGRA && s->format != NV_IOP_FMT_L10R && s->format != NV_IOP_FMT_RGHA)
        return NV_FRAME_FORMAT;
    row = (uint64_t)s->width * NV_IOP_BYTES_PER_PIXEL(s->format);
    full = (uint64_t)s->pitch * s->height;
    visible = (uint64_t)(s->height - 1u) * s->pitch + row;
    if (row > s->pitch || full > s->capacity || visible > s->capacity ||
        full > SIZE_MAX || (uintptr_t)s->address > UINTPTR_MAX - (uintptr_t)full) return NV_FRAME_BOUNDS;
    return NV_FRAME_OK;
}
static inline int nv_frame_populate(const struct nv_frame_span *source, const struct nv_frame_span *target)
{
    int r = nv_frame_validate(source);
    uint32_t y;
    if (r) return r;
    r = nv_frame_validate(target);
    if (r) return r;
    if (target->format != NV_IOP_FMT_BGRA) return NV_FRAME_FORMAT;
    if (source->width != target->width || source->height != target->height) return NV_FRAME_ARGUMENT;
    /* No row converter can fail after the complete format/span checks above. */
    for (y = 0; y < source->height; ++y) {
        const volatile unsigned *s = (const volatile unsigned *)((const unsigned char *)source->address + (size_t)y * source->pitch);
        volatile unsigned *d = (volatile unsigned *)((unsigned char *)(uintptr_t)target->address + (size_t)y * target->pitch);
        (void)nv_iop_convert_row(d, s, source->width, source->format, 0);
    }
    return NV_FRAME_OK;
}
static inline void nv_frame_store_drain(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("sfence\n\tmfence" ::: "memory");
#else
    __sync_synchronize();
#endif
}
