/*
 * Option C Hybrid-Without-NVIDIA-Display Policy Helpers for NVAccel
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#pragma once

#include <stdint.h>
#include "nm_hybrid.h"
#include "../nvrm_vram_abi.h"
#include "../nvrm_fb_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

enum nm_optc_claim_outcome {
    NM_OPTC_CLAIM_INVALID = -1,
    NM_OPTC_CLAIM_CONFLICT = 0,
    NM_OPTC_CLAIM_ACQUIRED = 1,
    NM_OPTC_CLAIM_IDEMPOTENT = 2
};

static inline enum nm_optc_claim_outcome nm_optc_decide_claim(
    const void *current_owner, const void *candidate)
{
    if (!candidate) return NM_OPTC_CLAIM_INVALID;
    if (current_owner == candidate) return NM_OPTC_CLAIM_IDEMPOTENT;
    if (current_owner != 0) return NM_OPTC_CLAIM_CONFLICT;
    return NM_OPTC_CLAIM_ACQUIRED;
}

static inline int nm_optc_should_set_primary_fb(enum nvrm_fb_kind kind, int logical_slot)
{
    return (kind == NVRM_FB_NVIDIA && logical_slot == 0) ? 1 : 0;
}

static inline int nm_optc_head4_assoc_valid(
    const void *slot4_fb, const void *pipe_fb,
    uint32_t dense_pipe_idx, uint32_t pipe_count, int is_inactive)
{
    return (slot4_fb != 0 && !is_inactive && pipe_fb == slot4_fb &&
            dense_pipe_idx < pipe_count && dense_pipe_idx < 16u) ? 1 : 0;
}

static inline int nm_optc_should_enable_igpu(int hybrid_level, int no_igpu_boot_arg)
{
    return (hybrid_level >= NMH_COPY && !no_igpu_boot_arg) ? 1 : 0;
}

static inline int nm_optc_has_active_head(
    int has_nvidia_fb, int hybrid_level, int igpu_enabled, int has_intel_fb)
{
    return (has_nvidia_fb || (hybrid_level >= NMH_COPY && igpu_enabled && has_intel_fb)) ? 1 : 0;
}

static inline int nm_optc_use_offscreen_fb_resource(
    int hybrid_level, const void *scanout_kva, uint64_t scanout_phys, int fb_is_nvidia)
{
    return (hybrid_level >= NMH_COPY && scanout_kva != 0 && scanout_phys == 0ULL && !fb_is_nvidia) ? 1 : 0;
}

static inline uint64_t nm_optc_scanout_backing_bytes(
    uint64_t actual_size, uint32_t pitch, uint32_t height)
{
    uint64_t bytes = actual_size;
    if (pitch && height) {
        if (pitch > 16384U * 8U || height > 16384U) return 0ULL;
        const uint64_t plane_bytes = (uint64_t)pitch * (uint64_t)height;
        if (plane_bytes > bytes) bytes = plane_bytes;
    }
    if (!bytes || bytes > (1ULL << 32)) return 0ULL;
    const uint64_t aligned = (bytes + 0xFFFULL) & ~0xFFFULL;
    return (aligned >= bytes) ? aligned : 0ULL;
}

static inline uint64_t nm_optc_vram_budget(uint64_t bar1_len)
{
    return (bar1_len >= (4ULL << 30)) ? (bar1_len / 2ULL) : NVRM_VRAM_BAR1_BUDGET;
}

static inline uint64_t nm_optc_align_vram_size(uint64_t want)
{
    if (!want || want > (1ULL << 34)) return 0ULL;
    const uint64_t aligned = (want + 0xFFFFULL) & ~0xFFFFULL;
    return (aligned >= want) ? aligned : 0ULL;
}

#ifdef __cplusplus
}
#endif
