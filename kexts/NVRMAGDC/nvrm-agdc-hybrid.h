/*
 * Hybrid (NMIntelFB / NMGopFB) + NVIDIA AGDC slot assignment policy.
 * Pure C, host-testable under ASan/UBSan with zero IOKit dependencies.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#ifndef NVRM_AGDC_HYBRID_H
#define NVRM_AGDC_HYBRID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVRM_AGDC_MAX_CANDIDATES 16u
#define NVRM_AGDC_MAX_SLOTS       5u

enum nvrm_agdc_fb_kind {
    NVRM_AGDC_FB_NATIVE = 0,
    NVRM_AGDC_FB_HYBRID = 1,
};

struct nvrm_agdc_candidate {
    uint64_t registry_id;
    enum nvrm_agdc_fb_kind kind;
    int has_accel_index;
    uint32_t accel_index;
    int has_dep_index;
    uint32_t dep_index;
    int matches_head4;
    int has_head4_dense;
    uint32_t head4_dense;
};

struct nvrm_agdc_plan {
    int slot_candidate[NVRM_AGDC_MAX_SLOTS];
    unsigned count;
    unsigned left_out_noidx;
    unsigned left_out_range;
    unsigned left_out_dup;
    unsigned left_out_hole;
};

static inline int nvrm_agdc_should_enroll_hybrid(
    int has_nvaccel_on_pci,
    uint64_t head4_reg_id,
    uint64_t fb_reg_id,
    int has_accel_index,
    int has_hybrid_prop)
{
    if (!has_nvaccel_on_pci || !fb_reg_id) return 0;
    if (head4_reg_id) return fb_reg_id == head4_reg_id;
    return (has_accel_index || has_hybrid_prop) ? 1 : 0;
}

static inline void nvrm_agdc_plan_slots(
    const struct nvrm_agdc_candidate *cands,
    unsigned ncands,
    unsigned max_slots,
    struct nvrm_agdc_plan *out)
{
    unsigned i;
    int placed[NVRM_AGDC_MAX_CANDIDATES];
    const unsigned limit = max_slots > NVRM_AGDC_MAX_SLOTS ? NVRM_AGDC_MAX_SLOTS : max_slots;
    if (!out) return;
    for (i = 0; i < NVRM_AGDC_MAX_SLOTS; ++i) out->slot_candidate[i] = -1;
    out->count = 0;
    out->left_out_noidx = 0;
    out->left_out_range = 0;
    out->left_out_dup = 0;
    out->left_out_hole = 0;
    if (!cands || !ncands || !limit) return;
    if (ncands > NVRM_AGDC_MAX_CANDIDATES) ncands = NVRM_AGDC_MAX_CANDIDATES;
    for (i = 0; i < ncands; ++i) placed[i] = 0;

    /* Pass 1: honor explicit dense IOAccelDisplayPipe ordinals first (plus verified head4 dense index). */
    for (i = 0; i < ncands; ++i) {
        const struct nvrm_agdc_candidate *c = &cands[i];
        uint32_t slot;
        if (c->has_accel_index) {
            slot = c->accel_index;
        } else if (c->kind == NVRM_AGDC_FB_HYBRID && c->matches_head4 && c->has_head4_dense) {
            slot = c->head4_dense;
        } else {
            continue;
        }
        placed[i] = 1;
        if (slot >= limit) {
            out->left_out_range++;
        } else if (out->slot_candidate[slot] >= 0) {
            out->left_out_dup++;
        } else {
            out->slot_candidate[slot] = (int)i;
        }
    }

    /* Pass 2: native NVIDIA framebuffers without IOAccelIndex yet use IOFBDependentIndex. */
    for (i = 0; i < ncands; ++i) {
        const struct nvrm_agdc_candidate *c = &cands[i];
        if (placed[i] || c->kind != NVRM_AGDC_FB_NATIVE) continue;
        placed[i] = 1;
        if (!c->has_dep_index) {
            out->left_out_noidx++;
            continue;
        }
        if (c->dep_index >= limit) {
            out->left_out_range++;
            continue;
        }
        if (out->slot_candidate[c->dep_index] < 0) {
            out->slot_candidate[c->dep_index] = (int)i;
        } else if (cands[out->slot_candidate[c->dep_index]].kind == NVRM_AGDC_FB_HYBRID) {
            unsigned s;
            int found = 0;
            for (s = 0; s < limit; ++s) {
                if (out->slot_candidate[s] < 0) {
                    out->slot_candidate[s] = (int)i;
                    found = 1;
                    break;
                }
            }
            if (!found) out->left_out_range++;
        } else {
            out->left_out_dup++;
        }
    }

    /* Pass 3: hybrid framebuffers without IOAccelIndex/head4_dense take the first free slot (no dep_index=0 collision). */
    for (i = 0; i < ncands; ++i) {
        unsigned s;
        int found = 0;
        if (placed[i] || cands[i].kind != NVRM_AGDC_FB_HYBRID) continue;
        placed[i] = 1;
        for (s = 0; s < limit; ++s) {
            if (out->slot_candidate[s] < 0) {
                out->slot_candidate[s] = (int)i;
                found = 1;
                break;
            }
        }
        if (!found) out->left_out_range++;
    }

    /* Pass 4: require contiguous prefix 0 .. count-1, rejecting any entries past a hole. */
    {
        unsigned n = 0;
        while (n < limit && out->slot_candidate[n] >= 0) n++;
        for (i = n; i < limit; ++i) {
            if (out->slot_candidate[i] >= 0) {
                out->left_out_hole++;
                out->slot_candidate[i] = -1;
            }
        }
        out->count = n;
    }
}

#ifdef __cplusplus
}
#endif

#endif
