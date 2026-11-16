/*
 * Option C GOP Console Scanout Validation for NMIntelFB
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Validate Apple PE_Video firmware console geometry when Intel PLANE_SURF GGTT
 * offset sits outside PCI BAR2 (e.g., Insyde V1.28 stolen memory at 0x70800000
 * mapped via GGTT 0xff000000 past a 256 MiB BAR2).
 */
static inline int nm_intelfb_console_ok(
    uint64_t base_addr, uint64_t width, uint64_t height,
    uint64_t row_bytes, uint64_t depth,
    uint32_t pipe_w, uint32_t pipe_h, uint32_t pipe_row_bytes,
    uint64_t *out_phys, uint64_t *out_bytes)
{
    const uint64_t phys = base_addr & ~3ULL;
    if (!phys || (phys & 0xFFFULL) != 0ULL) return 0;
    if (!pipe_w || !pipe_h || pipe_row_bytes < (uint64_t)pipe_w * 4ULL) return 0;
    if (pipe_w > 16384U || pipe_h > 16384U) return 0;
    if (width != (uint64_t)pipe_w || height != (uint64_t)pipe_h || depth != 32ULL) return 0;
    if (row_bytes != (uint64_t)pipe_row_bytes) return 0;
    const uint64_t bytes = (uint64_t)pipe_row_bytes * (uint64_t)pipe_h;
    if (!bytes || (phys + bytes) < phys) return 0;
    if (out_phys) *out_phys = phys;
    if (out_bytes) *out_bytes = bytes;
    return 1;
}

#ifdef __cplusplus
}
#endif
