/*
 * EXP-013 firmware authority and synchronous console publication policy.
 * Used by NMIntelFB and its host behavioral tests; no IOKit or MMIO side effects.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../NVRM/accel/nm_intelfb_present_abi.h"

#define NM_INTEL_EFI_MAP_MAX (4u * 1024u * 1024u)
#define NM_INTEL_PAGE 4096ull

struct NMIntelVideo {
    uint64_t base, width, height, pitch, depth, display, rotate;
};
struct NMIntelEfiMap {
    uint64_t address;
    uint32_t bytes, stride, version;
};
struct NMIntelPlane {
    uint32_t conf, control, stride, size, surface, live, position, offset, pipeSource, ggc;
    uint64_t gsmBase, dsmBase;
};

static inline int nm_intelfb_add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}
static inline int nm_intelfb_span(uint64_t width, uint64_t height, uint64_t pitch,
                                uint64_t capacity, uint64_t *full, uint64_t *visible)
{
    if (!width || !height || width > 16384 || height > 16384 ||
        width > UINT64_MAX / 4 || pitch < width * 4 || (pitch & 3) ||
        pitch > UINT64_MAX / height) return 0;
    const uint64_t all = pitch * height, used = (height - 1) * pitch + width * 4;
    if (all > capacity || used > capacity) return 0;
    if (full) *full = all;
    if (visible) *visible = used;
    return 1;
}
static inline int nm_intelfb_kernel_span(uint64_t address, uint64_t bytes)
{
    return address >= 0xffff800000000000ull && bytes && bytes <= UINT64_MAX - address;
}
static inline int nm_intelfb_boot_header_ok(uint32_t version, uint32_t revision, uint32_t efiMode)
{
    /* The pinned x86 boot_args ABI is version 2, revisions 0 and 1. */
    return version == 2 && revision <= 1 && efiMode == 64;
}
/* boot_args->Video.v_display is 1 (GRAPHICS_MODE) on normal boot and 2 (FB_TEXT_MODE) under -v;
 * XNU PE_create_console() sets PE_state.video.v_display = 1 without mutating PE_state.bootArgs,
 * so boot metadata permits {1, 2} while active console matching below keeps current->display == 1. */
static inline int nm_intelfb_video_ok(const struct NMIntelVideo *v, uint64_t *bytes,
                                    uint64_t *rounded)
{
    uint64_t size, end, pages;
    if (!v || !v->base || (v->base & (NM_INTEL_PAGE - 1)) ||
        v->base >= (1ull << 52) || v->width < 320 || v->height < 200 ||
        v->depth != 32 || (v->display != 1 && v->display != 2) || v->rotate ||
        !nm_intelfb_span(v->width, v->height, v->pitch, UINT64_MAX, &size, 0) ||
        !nm_intelfb_add(size, NM_INTEL_PAGE - 1, &pages)) return 0;
    pages &= ~(NM_INTEL_PAGE - 1);
    if (!nm_intelfb_add(v->base, pages, &end) || end > (1ull << 52)) return 0;
    if (bytes) *bytes = size;
    if (rounded) *rounded = pages;
    return 1;
}
static inline int nm_intelfb_video_equal(const struct NMIntelVideo *a, const struct NMIntelVideo *b)
{
    return a->base == b->base && a->width == b->width && a->height == b->height &&
           a->pitch == b->pitch && a->depth == b->depth && a->display == b->display &&
           a->rotate == b->rotate;
}
static inline int nm_intelfb_console_matches(const struct NMIntelVideo *original,
                                           const struct NMIntelVideo *current,
                                           uint64_t ownShadow)
{
    if (!original || !current || current->width != original->width ||
        current->height != original->height || current->pitch != original->pitch ||
        current->depth != 32 || current->display != 1 || current->rotate) return 0;
    /* Never turn an unfamiliar kernel VA into physical authority by masking it. */
    return current->base == original->base || current->base == (original->base | 1ull) ||
           (ownShadow && current->base == ownShadow);
}
static inline int nm_intelfb_efi_metadata_ok(const struct NMIntelEfiMap *m)
{
    uint64_t end;
    return m && m->address && m->bytes && m->bytes <= NM_INTEL_EFI_MAP_MAX &&
           m->stride >= 40 && !(m->stride & 7) && m->bytes % m->stride == 0 &&
           m->version == 1 && nm_intelfb_add(m->address, m->bytes, &end) &&
           end <= (1ull << 32);
}
static inline int nm_intelfb_efi_metadata_equal(const struct NMIntelEfiMap *a,
                                             const struct NMIntelEfiMap *b)
{
    return a->address == b->address && a->bytes == b->bytes &&
           a->stride == b->stride && a->version == b->version;
}
static inline uint32_t nm_intelfb_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t nm_intelfb_u64(const uint8_t *p)
{
    return nm_intelfb_u32(p) | ((uint64_t)nm_intelfb_u32(p + 4) << 32);
}
static inline int nm_intelfb_efi_range(const uint8_t *p, uint64_t *lo, uint64_t *hi)
{
    const uint64_t pages = nm_intelfb_u64(p + 24);
    *lo = nm_intelfb_u64(p + 8);
    return pages && pages <= UINT64_MAX / NM_INTEL_PAGE &&
           !(*lo & (NM_INTEL_PAGE - 1)) &&
           nm_intelfb_add(*lo, pages * NM_INTEL_PAGE, hi) && *hi <= (1ull << 52);
}
static inline int nm_intelfb_reserved(const struct NMIntelEfiMap *m, const uint8_t *map,
                                    uint64_t base, uint64_t roundedBytes)
{
    uint64_t end, covered = 0;
    if (!nm_intelfb_efi_metadata_ok(m) || !map || !roundedBytes ||
        (base & (NM_INTEL_PAGE - 1)) || (roundedBytes & (NM_INTEL_PAGE - 1)) ||
        !nm_intelfb_add(base, roundedBytes, &end)) return 0;
    for (uint32_t i = 0; i < m->bytes; i += m->stride) {
        uint64_t lo, hi;
        if (!nm_intelfb_efi_range(map + i, &lo, &hi)) return 0;
        if (hi <= base || lo >= end) continue;
        /* Only firmware-reserved, non-runtime pages may back the console. */
        if (nm_intelfb_u32(map + i) != 0 || (nm_intelfb_u64(map + i + 32) >> 63)) return 0;
        const uint64_t first = lo > base ? lo : base, last = hi < end ? hi : end;
        for (uint32_t j = 0; j < i; j += m->stride) {
            uint64_t otherLo, otherHi;
            if (!nm_intelfb_efi_range(map + j, &otherLo, &otherHi)) return 0;
            if (otherLo < last && otherHi > first) return 0;
        }
        covered += last - first; /* nonoverlap keeps this <= roundedBytes */
    }
    return covered == roundedBytes;
}

/* Reader returns false for any inaccessible offset. BAR2 is never consulted. */
typedef int (*NMIntelRead32)(void *, uint64_t, uint32_t *);
static inline int nm_intelfb_read32(NMIntelRead32 read, void *ctx, uint64_t barBytes,
                                   uint64_t offset, uint32_t *out)
{
    return read && !(offset & 3) && offset <= barBytes && barBytes - offset >= 4 &&
           read(ctx, offset, out);
}
static inline int nm_intelfb_read64(NMIntelRead32 read, void *ctx, uint64_t barBytes,
                                   uint64_t offset, uint64_t *out)
{
    uint32_t lo, hi;
    if (offset > UINT64_MAX - 4 ||
        !nm_intelfb_read32(read, ctx, barBytes, offset, &lo) ||
        !nm_intelfb_read32(read, ctx, barBytes, offset + 4, &hi)) return 0;
    *out = lo | ((uint64_t)hi << 32);
    return 1;
}
static inline int nm_intelfb_plane_snapshot(NMIntelRead32 read, void *ctx, uint64_t barBytes,
                                          unsigned pipe, struct NMIntelPlane *p)
{
    if (!p || pipe >= 4) return 0;
    const uint64_t delta = pipe * 0x1000ull;
    return nm_intelfb_read32(read, ctx, barBytes, 0x70008 + delta, &p->conf) &&
           nm_intelfb_read32(read, ctx, barBytes, 0x70180 + delta, &p->control) &&
           nm_intelfb_read32(read, ctx, barBytes, 0x70188 + delta, &p->stride) &&
           nm_intelfb_read32(read, ctx, barBytes, 0x70190 + delta, &p->size) &&
           nm_intelfb_read32(read, ctx, barBytes, 0x7019c + delta, &p->surface) &&
           nm_intelfb_read32(read, ctx, barBytes, 0x701ac + delta, &p->live) &&
           nm_intelfb_read32(read, ctx, barBytes, 0x7018c + delta, &p->position) &&
           nm_intelfb_read32(read, ctx, barBytes, 0x701a4 + delta, &p->offset) &&
           nm_intelfb_read32(read, ctx, barBytes, 0x6001c + delta, &p->pipeSource) &&
           nm_intelfb_read32(read, ctx, barBytes, 0x108040, &p->ggc) &&
           nm_intelfb_read64(read, ctx, barBytes, 0x108100, &p->gsmBase) &&
           nm_intelfb_read64(read, ctx, barBytes, 0x1080c0, &p->dsmBase);
}
static inline int nm_intelfb_plane_equal(const struct NMIntelPlane *a, const struct NMIntelPlane *b)
{
    return a->conf == b->conf && a->control == b->control && a->stride == b->stride &&
           a->size == b->size && a->surface == b->surface && a->live == b->live &&
           a->position == b->position && a->offset == b->offset &&
           a->pipeSource == b->pipeSource && a->ggc == b->ggc &&
           a->gsmBase == b->gsmBase && a->dsmBase == b->dsmBase;
}
static inline int nm_intelfb_plane_ok(const struct NMIntelPlane *p, const struct NMIntelVideo *v)
{
    /* Linear layout does not use a DPT, even when the general DPT-disable bit is clear. */
    const uint32_t unsupported = (7u << 10) | (1u << 15) | (1u << 4) |
                                 (1u << 8) | (1u << 20) | 3u;
    return (p->conf & (1u << 31)) && (p->control & (1u << 31)) &&
           ((p->control >> 23) & 31) == 8 && !(p->control & unsupported) &&
           !p->position && !p->offset &&
           (uint64_t)((p->size & 0xffff) + 1) == v->width &&
           (uint64_t)((p->size >> 16) + 1) == v->height &&
           (uint64_t)((p->pipeSource >> 16) + 1) == v->width &&
           (uint64_t)((p->pipeSource & 0xffff) + 1) == v->height &&
           (uint64_t)(p->stride & 0xfff) * 64 == v->pitch &&
           (p->surface & ~0xfffu) == (p->live & ~0xfffu);
}
static inline int nm_intelfb_translation(NMIntelRead32 read, void *ctx, uint64_t barBytes,
                                       unsigned displayVersion, unsigned pipe,
                                       const struct NMIntelVideo *v, struct NMIntelPlane *accepted)
{
    struct NMIntelPlane before, after;
    uint64_t bytes, rounded;
    if (displayVersion < 12 || displayVersion > 14 ||
        !nm_intelfb_video_ok(v, &bytes, &rounded) ||
        !nm_intelfb_plane_snapshot(read, ctx, barBytes, pipe, &before) ||
        !nm_intelfb_plane_ok(&before, v)) return 0;
    const unsigned ggms = (before.ggc >> 6) & 3;
    const uint64_t tableBytes = ggms ? (1ull << ggms) << 20 : 0;
    const uint64_t tableOffset = 8ull << 20;
    const uint64_t first = (before.live & ~0xfffull) / NM_INTEL_PAGE;
    const uint64_t pages = rounded / NM_INTEL_PAGE;
    if (!tableBytes || tableOffset > barBytes || tableBytes > barBytes - tableOffset ||
        first >= tableBytes / 8 || pages > tableBytes / 8 - first) return 0;
    const uint64_t gsm = before.gsmBase & ~0xfffffull;
    const uint64_t dsm = before.dsmBase & ~0xfffffull;
    if (displayVersion == 14 && (ggms != 3 || !gsm || gsm >= (1ull << 52) ||
        gsm > UINT64_MAX - tableBytes || dsm != gsm + tableBytes)) return 0;
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t pte, again, physical;
        const uint64_t at = tableOffset + (first + i) * 8;
        if (!nm_intelfb_read64(read, ctx, barBytes, at, &pte) ||
            !nm_intelfb_read64(read, ctx, barBytes, at, &again) || pte != again || !(pte & 1)) return 0;
        physical = pte & 0x00003ffffffff000ull; /* address bits 45:12 */
        if (pte & 2) {
            if (displayVersion != 14 || !nm_intelfb_add(gsm, physical, &physical)) return 0;
        }
        if (physical != v->base + i * NM_INTEL_PAGE) return 0;
    }
    if (!nm_intelfb_plane_snapshot(read, ctx, barBytes, pipe, &after) ||
        !nm_intelfb_plane_equal(&before, &after)) return 0;
    if (accepted) *accepted = after;
    return 1;
}

enum NMIntelPhase { NMIntelPrepared, NMIntelApertureActive, NMIntelRecovery, NMIntelStopped };
struct NMIntelPresentState {
    enum NMIntelPhase phase;
    uint64_t epoch, owner, sequence;
};
static inline int nm_intelfb_revoke(struct NMIntelPresentState *s, enum NMIntelPhase next)
{
    if (s->phase == NMIntelStopped) return 0;
    s->owner = s->sequence = 0;
    if (s->epoch == UINT64_MAX) { s->phase = NMIntelStopped; return 0; }
    ++s->epoch;
    s->phase = next;
    return 1;
}
static inline int nm_intelfb_activate(struct NMIntelPresentState *s)
{
    if (!s->epoch || s->phase == NMIntelStopped) return 0;
    s->phase = NMIntelApertureActive;
    return 1;
}
enum NMIntelPresentResult {
    NMIntelPresentOK, NMIntelPresentBadArgument, NMIntelPresentNotReady,
    NMIntelPresentStale, NMIntelPresentUnsupported, NMIntelPresentOverrun
};
static inline enum NMIntelPresentResult nm_intelfb_present_check(
    const struct NMIntelPresentState *s, uint64_t registryID,
    uint32_t width, uint32_t height, uint32_t pitch, uint64_t shadowBytes, uint64_t sinkBytes,
    const struct NMIntelPresentRequest *q)
{
    if (!q || q->version != NM_INTEL_FB_ABI_VERSION || q->structSize != sizeof(*q) ||
        !q->sequence || !q->sourceOwnerGeneration || !q->sourceAllocationCookie ||
        !q->sourceKva || ((uintptr_t)q->sourceKva & 3)) return NMIntelPresentBadArgument;
    if (s->phase != NMIntelApertureActive) return NMIntelPresentNotReady;
    if (q->framebufferRegistryID != registryID || q->epoch != s->epoch ||
        (s->owner && (q->sourceOwnerGeneration < s->owner ||
         (q->sourceOwnerGeneration == s->owner && q->sequence <= s->sequence))))
        return NMIntelPresentStale;
    if (q->format != NM_INTEL_FB_FORMAT_BGRA || q->width != width || q->height != height)
        return NMIntelPresentUnsupported;
    if (!nm_intelfb_span(width, height, q->pitch, q->sourceBytes, 0, 0) ||
        !nm_intelfb_span(width, height, pitch, shadowBytes, 0, 0) ||
        !nm_intelfb_span(width, height, pitch, sinkBytes, 0, 0)) return NMIntelPresentOverrun;
    return NMIntelPresentOK;
}
/* All spans are checked before the first store. One load feeds BOTH destinations. */
#if defined(__clang__) || defined(__GNUC__)
typedef uint64_t __attribute__((__may_alias__)) nm_intelfb_u64_a;
typedef uint32_t __attribute__((__may_alias__)) nm_intelfb_u32_a;
#else
typedef uint64_t nm_intelfb_u64_a;
typedef uint32_t nm_intelfb_u32_a;
#endif

static inline int nm_intelfb_ranges_overlap(uintptr_t aLo, uintptr_t aHi, uintptr_t bLo, uintptr_t bHi)
{
    return aLo < bHi && bLo < aHi;
}

static inline int nm_intelfb_copy_final_diff(const void *source, uint64_t sourceBytes, uint32_t sourcePitch,
                                             void *shadow, uint64_t shadowBytes, void *sink, uint64_t sinkBytes,
                                             void *mirror, uint64_t mirrorBytes, int *mirrorValid,
                                             uint32_t width, uint32_t height, uint32_t pitch)
{
    uint64_t sourceFull, targetFull;
    if (!source || !shadow || ((uintptr_t)source & 3) || ((uintptr_t)shadow & 3) ||
        (sink && ((uintptr_t)sink & 3)) ||
        !nm_intelfb_span(width, height, sourcePitch, sourceBytes, &sourceFull, 0) ||
        !nm_intelfb_span(width, height, pitch, shadowBytes, &targetFull, 0) ||
        !nm_intelfb_span(width, height, pitch, sinkBytes, 0, 0) ||
        sourceFull > UINTPTR_MAX - (uintptr_t)source ||
        targetFull > UINTPTR_MAX - (uintptr_t)shadow ||
        (sink && targetFull > UINTPTR_MAX - (uintptr_t)sink)) return 0;
    if (mirror) {
        if (!mirrorValid || ((uintptr_t)mirror & 3) ||
            !nm_intelfb_span(width, height, pitch, mirrorBytes, 0, 0) ||
            targetFull > UINTPTR_MAX - (uintptr_t)mirror) return 0;
        const uintptr_t mLo = (uintptr_t)mirror, mHi = mLo + (size_t)targetFull;
        const uintptr_t sLo = (uintptr_t)source, sHi = sLo + (size_t)sourceFull;
        const uintptr_t dLo = (uintptr_t)shadow, dHi = dLo + (size_t)targetFull;
        if (nm_intelfb_ranges_overlap(mLo, mHi, sLo, sHi) ||
            nm_intelfb_ranges_overlap(mLo, mHi, dLo, dHi)) return 0;
        if (sink) {
            const uintptr_t oLo = (uintptr_t)sink, oHi = oLo + (size_t)targetFull;
            if (nm_intelfb_ranges_overlap(mLo, mHi, oLo, oHi)) return 0;
        }
    }
    const int updateShadow = source != shadow || sourcePitch != pitch;
    const int useMirror = mirror != 0;
    const int valid = useMirror && (*mirrorValid != 0);
    const int aligned8 = (((uintptr_t)source | sourcePitch | (uintptr_t)shadow | pitch |
                           (uintptr_t)sink | (uintptr_t)mirror) & 7u) == 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *sRow = (const uint8_t *)source + (uint64_t)y * sourcePitch;
        uint8_t *dRow = (uint8_t *)shadow + (uint64_t)y * pitch;
        uint8_t *oRow = sink ? (uint8_t *)sink + (uint64_t)y * pitch : 0;
        uint8_t *mRow = mirror ? (uint8_t *)mirror + (uint64_t)y * pitch : 0;
        uint32_t x = 0;
        if (aligned8 && width >= 2u) {
            const uint32_t pairs = width >> 1;
            const volatile nm_intelfb_u64_a *src64 = (const volatile nm_intelfb_u64_a *)sRow;
            volatile nm_intelfb_u64_a *dst64 = (volatile nm_intelfb_u64_a *)dRow;
            volatile nm_intelfb_u64_a *out64 = oRow ? (volatile nm_intelfb_u64_a *)oRow : 0;
            nm_intelfb_u64_a *mir64 = mRow ? (nm_intelfb_u64_a *)mRow : 0;
            if (mir64) {
                uint32_t i = 0;
                if (valid) {
                    const uint32_t quads = pairs & ~3u;
                    for (; i < quads; i += 4u) {
                        const uint64_t w0 = src64[i], w1 = src64[i + 1u], w2 = src64[i + 2u], w3 = src64[i + 3u];
                        if ((mir64[i] ^ w0) | (mir64[i + 1u] ^ w1) |
                            (mir64[i + 2u] ^ w2) | (mir64[i + 3u] ^ w3)) {
                            if (mir64[i] != w0) { mir64[i] = w0; if (updateShadow) dst64[i] = w0; if (out64) out64[i] = w0; }
                            if (mir64[i + 1u] != w1) { mir64[i + 1u] = w1; if (updateShadow) dst64[i + 1u] = w1; if (out64) out64[i + 1u] = w1; }
                            if (mir64[i + 2u] != w2) { mir64[i + 2u] = w2; if (updateShadow) dst64[i + 2u] = w2; if (out64) out64[i + 2u] = w2; }
                            if (mir64[i + 3u] != w3) { mir64[i + 3u] = w3; if (updateShadow) dst64[i + 3u] = w3; if (out64) out64[i + 3u] = w3; }
                        }
                    }
                }
                for (; i < pairs; ++i) {
                    const uint64_t w = src64[i];
                    if (!valid || mir64[i] != w) {
                        mir64[i] = w;
                        if (updateShadow) dst64[i] = w;
                        if (out64) out64[i] = w;
                    }
                }
            } else {
                for (uint32_t i = 0; i < pairs; ++i) {
                    const uint64_t w = src64[i];
                    if (updateShadow) dst64[i] = w;
                    if (out64) out64[i] = w;
                }
            }
            x = pairs << 1;
        }
        {
            const volatile nm_intelfb_u32_a *src = (const volatile nm_intelfb_u32_a *)sRow;
            volatile nm_intelfb_u32_a *dst = (volatile nm_intelfb_u32_a *)dRow;
            volatile nm_intelfb_u32_a *out = oRow ? (volatile nm_intelfb_u32_a *)oRow : 0;
            nm_intelfb_u32_a *mir = mRow ? (nm_intelfb_u32_a *)mRow : 0;
            for (; x < width; ++x) {
                const uint32_t pixel = src[x];
                if (mir) {
                    if (!valid || mir[x] != pixel) {
                        mir[x] = pixel;
                        if (updateShadow) dst[x] = pixel;
                        if (out) out[x] = pixel;
                    }
                } else {
                    if (updateShadow) dst[x] = pixel;
                    if (out) out[x] = pixel;
                }
            }
        }
    }
    if (mirrorValid) *mirrorValid = useMirror ? 1 : 0;
    return 1;
}

static inline int nm_intelfb_copy_final(const void *source, uint64_t sourceBytes, uint32_t sourcePitch,
                                      void *shadow, uint64_t shadowBytes, void *sink, uint64_t sinkBytes,
                                      uint32_t width, uint32_t height, uint32_t pitch)
{
    return nm_intelfb_copy_final_diff(source, sourceBytes, sourcePitch,
                                      shadow, shadowBytes,
                                      sink, sinkBytes,
                                      0, 0, 0,
                                      width, height, pitch);
}

#define NM_INTEL_NVRMTIMING_TAG   0x4b350001u
#define NM_INTEL_NVRMTIMING_WORDS 13u

static inline int nm_intelfb_build_nvrm_timing(
    uint32_t width, uint32_t height,
    uint32_t hss, uint32_t hse, uint32_t htot,
    uint32_t vss, uint32_t vse, uint32_t vtot,
    uint64_t pclkHz, uint32_t refreshMilliHz,
    uint32_t hSyncPos, uint32_t vSyncPos,
    uint32_t out[NM_INTEL_NVRMTIMING_WORDS])
{
    if (!out || !width || !height) return 0;
    if (htot <= width) htot = width + 160u;
    if (!(width <= hss && hss < hse && hse <= htot)) {
        const uint32_t blank = htot - width;
        hss = width + blank / 4u;
        hse = hss + (blank >= 4u ? blank / 2u : 1u);
        if (hse > htot) { hss = width; hse = width + 1u; if (hse > htot) htot = hse; }
    }
    if (vtot <= height) vtot = height + 45u;
    if (!(height <= vss && vss < vse && vse <= vtot)) {
        const uint32_t blank = vtot - height;
        vss = height + blank / 4u;
        vse = vss + (blank >= 4u ? blank / 2u : 1u);
        if (vse > vtot) { vss = height; vse = height + 1u; if (vse > vtot) vtot = vse; }
    }
    if (!refreshMilliHz) refreshMilliHz = 60000u;
    if (!pclkHz || pclkHz > UINT32_MAX) {
        const uint64_t hz = refreshMilliHz >= 1000u ? (refreshMilliHz / 1000u) : 60u;
        pclkHz = (uint64_t)htot * (uint64_t)vtot * hz;
        if (pclkHz > UINT32_MAX) pclkHz = UINT32_MAX;
        if (!pclkHz) pclkHz = 1u;
    }
    out[0]  = NM_INTEL_NVRMTIMING_TAG;
    out[1]  = (uint32_t)pclkHz;
    out[2]  = width;
    out[3]  = hss;
    out[4]  = hse;
    out[5]  = htot;
    out[6]  = height;
    out[7]  = vss;
    out[8]  = vse;
    out[9]  = vtot;
    out[10] = refreshMilliHz;
    out[11] = hSyncPos ? 1u : 0u;
    out[12] = vSyncPos ? 1u : 0u;
    return 1;
}

static inline uint64_t nm_intelfb_refresh_gap(uint64_t copyDuration, uint64_t minInterval)
{
    if (copyDuration > UINT64_MAX / 4u) return UINT64_MAX;
    const uint64_t adaptive = copyDuration * 4u;
    return adaptive > minInterval ? adaptive : minInterval;
}

static inline int nm_intelfb_should_refresh(uint64_t accelSequence, uint64_t now, uint64_t nextRefresh)
{
    return accelSequence == 0 && now >= nextRefresh;
}
static inline void nm_intelfb_store_drain(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("sfence\n\tmfence" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static inline void nm_intelfb_present_complete(struct NMIntelPresentState *s, struct NMIntelPresentRequest *q)
{
    nm_intelfb_store_drain();
    s->owner = q->sourceOwnerGeneration;
    s->sequence = q->sequence;
    q->completedSequence = q->sequence;
}
static inline int nm_intelfb_shadow_cache_ok(uint32_t options, uint32_t cacheMask,
                                           uint32_t defaultCache, uint32_t writeCombineCache)
{
    const uint32_t cache = options & cacheMask;
    return cache == defaultCache || cache == writeCombineCache;
}
