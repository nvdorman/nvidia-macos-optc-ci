/* Kernel-only owner coordination. Public VRAM/GPUVA/flip ABIs stay unchanged. */
#pragma once
#include <stdint.h>
#include <IOKit/IOReturn.h>
class IOService;

#define NVRM_OWNER_ABI_VERSION 1u
#define NVRM_KERNEL_MAP_INFO_FN "nvKernelMapInfo"
#define NVRM_OWNER_CPU_ACCESS_FN "nvOwnerCpuAccess"
#define NVRM_GPUVA_DRAIN_FN "nvGpuVaDrain"
#define NVRM_ACCEL_TARGET_FN "nvAccelTarget"
#define NVRM_OWNER_GENERATION_PROPERTY "NVVramOwnerGeneration"

enum { NVRM_ACCEL_TARGET_REGISTER = 1u, NVRM_ACCEL_TARGET_UNREGISTER = 2u };

struct NVRMKernelMapInfo {
    uint32_t version, structSize;
    const void *address;
    uint64_t requiredBytes;
    uint64_t physical, availableBytes;
    uint32_t cacheOptions, reserved;
};

struct NVRMOwnerCpuAccess {
    uint32_t version, structSize;
    IOService *owner;
    uint64_t ownerGeneration;
    IOReturn (*body)(void *);
    void *context;
};

struct NVRMAccelTarget {
    uint32_t version, structSize;
    uint32_t operation, reserved;
    IOService *target;
    uint64_t generation;
};

static_assert(sizeof(NVRMKernelMapInfo) == 48, "kernel mapping receipt ABI");
static_assert(sizeof(NVRMOwnerCpuAccess) == 40, "synchronous CPU-use ABI");
static_assert(sizeof(NVRMAccelTarget) == 32, "accelerator target ABI");
