/* Kernel-only synchronous final-image publication; no destination pointer escapes. */
#pragma once
#include <stdint.h>
#define NM_INTEL_FB_INFO_FN "nmIntelFramebufferInfo"
#define NM_INTEL_FB_PRESENT_FN "nmIntelPresent"
#define NM_INTEL_FB_ABI_VERSION 1u
#define NM_INTEL_FB_FORMAT_BGRA 0x42475241u

struct NMIntelFramebufferInfo {
    uint32_t version, structSize;
    uint64_t framebufferRegistryID, epoch, actualSize;
    uint32_t width, height, pitch, format;
};

struct NMIntelPresentRequest {
    uint32_t version, structSize;
    uint64_t framebufferRegistryID, epoch;
    uint64_t sourceOwnerGeneration, sourceAllocationCookie, sequence;
    const void *sourceKva;
    uint64_t sourceBytes;
    uint32_t width, height, pitch, format;
    uint64_t completedSequence;
};

#ifdef __cplusplus
static_assert(sizeof(NMIntelFramebufferInfo) == 48, "Intel info ABI");
static_assert(sizeof(NMIntelPresentRequest) == 88, "Intel present ABI");
#endif
