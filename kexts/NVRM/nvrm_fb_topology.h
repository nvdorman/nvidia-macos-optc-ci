/*
 * NullMoth NVIDIA driver for macOS — Framebuffer Topology Classification
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Separates three distinct display namespaces:
 *   1. Controller identity (IOFBDependentID) + controller-local index (IOFBDependentIndex)
 *   2. Logical NVAccel slot (0..3 NVIDIA heads, 4 hybrid foreign panel)
 *   3. Dense Apple IOAccelerator pipe index (0..N-1 in registration order)
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVRM_FB_CONTROLLER_ID_NVIDIA 0x4E56524D00000001ULL
#define NVRM_FB_CONTROLLER_ID_INTEL  0x4E4D494E00000001ULL

#define NVRM_FB_MAX_NVIDIA_HEADS     4u
#define NVRM_FB_INTEL_LOCAL_INDEX    0u
#define NVRM_FB_GOP_LOCAL_INDEX      4u
#define NVRM_FB_LOGICAL_HYBRID_HEAD  4
#define NVRM_FB_MAX_HEADS            5u
#define NVRM_FB_HYBRID_PUBLISHED_BIT (1u << 4)

enum nvrm_fb_kind {
    NVRM_FB_UNKNOWN,
    NVRM_FB_NVIDIA,
    NVRM_FB_INTEL,
    NVRM_FB_GOP
};

static inline int nvrm_fb_logical_head(
    enum nvrm_fb_kind kind,
    int has_controller_id, uint64_t controller_id,
    int has_local_index, uint32_t local_index)
{
    if (!has_controller_id || !has_local_index)
        return -1;

    switch (kind) {
    case NVRM_FB_NVIDIA:
        if (controller_id != NVRM_FB_CONTROLLER_ID_NVIDIA)
            return -1;
        if (local_index >= NVRM_FB_MAX_NVIDIA_HEADS)
            return -1;
        return (int)local_index;

    case NVRM_FB_INTEL:
        if (controller_id != NVRM_FB_CONTROLLER_ID_INTEL)
            return -1;
        if (local_index != NVRM_FB_INTEL_LOCAL_INDEX)
            return -1;
        return NVRM_FB_LOGICAL_HYBRID_HEAD;

    case NVRM_FB_GOP:
        if (controller_id != NVRM_FB_CONTROLLER_ID_NVIDIA)
            return -1;
        if (local_index != NVRM_FB_GOP_LOCAL_INDEX)
            return -1;
        return NVRM_FB_LOGICAL_HYBRID_HEAD;

    case NVRM_FB_UNKNOWN:
    default:
        return -1;
    }
}

enum nvrm_fb_rtd3_role {
    NVRM_RTD3_FB_OTHER = 0,
    NVRM_RTD3_FB_OWN = 1,
    NVRM_RTD3_FB_FOREIGN = 2,
    NVRM_RTD3_FB_UNVERIFIED = 3
};

static inline enum nvrm_fb_rtd3_role nvrm_fb_classify_for_rtd3(
    enum nvrm_fb_kind kind,
    int has_controller_id, uint64_t controller_id, int controller_id_valid_type,
    int has_local_index, uint32_t local_index, int local_index_valid_type)
{
    switch (kind) {
    case NVRM_FB_NVIDIA:
        return NVRM_RTD3_FB_OWN;

    case NVRM_FB_INTEL:
    case NVRM_FB_GOP:
        if (!has_controller_id || !controller_id_valid_type ||
            !has_local_index || !local_index_valid_type)
            return NVRM_RTD3_FB_UNVERIFIED;
        return (nvrm_fb_logical_head(kind, 1, controller_id, 1, local_index) == NVRM_FB_LOGICAL_HYBRID_HEAD)
                   ? NVRM_RTD3_FB_FOREIGN
                   : NVRM_RTD3_FB_UNVERIFIED;

    case NVRM_FB_UNKNOWN:
    default:
        if (has_controller_id && !controller_id_valid_type)
            return NVRM_RTD3_FB_UNVERIFIED;
        if (has_controller_id && controller_id == NVRM_FB_CONTROLLER_ID_INTEL)
            return NVRM_RTD3_FB_UNVERIFIED;
        if (has_controller_id && controller_id == NVRM_FB_CONTROLLER_ID_NVIDIA)
            return NVRM_RTD3_FB_FOREIGN;
        return NVRM_RTD3_FB_OTHER;
    }
}

static inline int nvrm_rtd3_evaluate_heads(
    int enumeration_ok, int own, int foreign, int unverified, int hybrid_level)
{
    if (!enumeration_ok || unverified > 0)
        return 1;
    return own + (hybrid_level > 0 ? foreign : 0);
}

#ifdef __cplusplus
}
#endif
