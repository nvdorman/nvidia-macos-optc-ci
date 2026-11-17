/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

// Apple's macOS SDK Kernel.framework omits MACH_KERNEL_PRIVATE so vm_map_t defaults to ipc_port_t,
// whereas com.apple.kpi.iokit exports IOGeneralMemoryDescriptor::doMap/doUnmap taking struct _vm_map *.
// Match the kernel KPI ABI before nvrm_off.h pulls in IOKit headers.
#include <IOKit/IOTypes.h>
struct _vm_map;
#pragma push_macro("vm_map_t")
#define vm_map_t struct _vm_map *
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#pragma pop_macro("vm_map_t")

#include "../nvrm_off.h"
#include "../nvrm_startup_policy.h"
#ifdef NMX_ACCEL
// NMXAccel: this accelerator built for the AMD and Intel GPUs that NMXGPU drives (see nmx_accel_names.h)
#include "nmx_accel_names.h"
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
// IOPCIDevice (with its setBusMasterEnable) is marked deprecated in favour of PCIDriverKit, which runs in user space and
// cannot replace a kernel extension's PCI access; that deprecation is ignored from here on in this kext's sources.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread_call.h>
#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelTask.h"
#include "IOAccelMemoryMap.h"
// Where Apple's video-memory object keeps its physical address (the field its own withPhysicalAddress and
// getPhysicalSegment use): +0xc8 from 10.15 on, +0xb8 on 10.12 to 10.14, whose memory base class is 16 bytes smaller,
// +0x78 on 10.11 (all three of the family's own methods that touch it agree on each release).
#if defined(NM_ERA) && NM_ERA < 1012
enum { kNvVidMemPhysOff = 0x78 };
#elif defined(NM_ERA) && NM_ERA < 1015
enum { kNvVidMemPhysOff = 0xb8 };
#else
enum { kNvVidMemPhysOff = 0xc8 };
#endif
#include "../nvrm_gpuva_abi.h"
#include "../nvrm_owner_abi.h"
#include "nvaccel_lock_owner.h"
#include "../nvrm_devtab.h"
#include "../nvrm_utilization_abi.h"
class NVMemoryMap;
class NVAccel;
static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map,
                                           unsigned long long len, unsigned long long align);
static void               nvAccelGpuVaFree(IOAccelMemoryMap *map);
static bool               nvAccelFlipTo(IOAccelResource2 *res);
static bool               nvAccelCopyToScanout(IOAccelResource2 *res);
void                      nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2);
bool                      nvAccelResHasContent(IOAccelResource2 *res);
static void               nvAccelWriteStamp(int index, unsigned int value);
static bool nvFamLock(NVAccel *, bool);
static void nvFamUnlock(NVAccel *);
class NVDisplayPipe;
static void nvAccelDrainPresent(NVAccel *);
static void nvAccelDrainPipe(NVDisplayPipe *);
struct NVRetain {
    OSObject *object;
    bool display = false;
    explicit NVRetain(OSObject *adopted) : object(adopted) {}
    explicit NVRetain(NVAccel *);
    ~NVRetain();
    NVRetain(const NVRetain &) = delete;
    NVRetain &operator=(const NVRetain &) = delete;
};
#ifdef NMX_ACCEL
#define NV_LOG_TAG "NMXAccel: "
#else
#define NV_LOG_TAG "NVAccel: "
#endif
#define ALOG(fmt, ...) do { kprintf(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); IOLog(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); } while (0)
// Per-allocation lines: WindowServer and every Metal app create resources continuously, and logging each one filled the
// system log (#242). They print only with the -nvaccelverbose boot-arg.
static bool nvaccel_verbose() { static int v = -1; if (v < 0) { int x = 0; v = PE_parse_boot_argn("-nvaccelverbose", &x, sizeof x) ? 1 : 0; } return v == 1; }
#define ALOGV(fmt, ...) do { if (nvaccel_verbose()) ALOG(fmt, ##__VA_ARGS__); } while (0)
#include <libkern/c++/OSSymbol.h>
#include "nvrm_vram_abi.h"
static_assert(sizeof(NVRMVramRequest) == 80 && __builtin_offsetof(NVRMVramRequest, allocationCookie) == 72, "stock VRAMv2 ABI");
#include <IOKit/IODeviceMemory.h>
#include "tahoe_fwd.h"   // macOS 26: inherited slots Apple no longer exports, forwarded through its vtables
#include "nvaccel_stubs.h"
// NVIDIA's open-gpu-kernel-modules headers (third-party) carry their own integer-conversion warnings under the kernel
// compiler's defaults; the exception is scoped to these includes and our code below stays checked.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wc99-designator"
#include "nvkms-kapi.h"
#pragma clang diagnostic pop
#include <sys/sysctl.h>
#include <libkern/OSAtomic.h>
#include "nv_iop_pixel.h"
#include "nm_hybrid.h"
#include "nm_hybrid_abi.h"
#include "nm_hybrid_p2p.h"
#include "../nvrm_fb_topology.h"
#include "nvaccel_optc_policy.h"

// ---- GPU core temperature, published into PerformanceStatistics -------------
// Same register the out-of-tree NvidiaSensor.kext reads. nouveau's gp100_temp_get() reads
// 0x020460 as well, but it insists on the "valid" bit (0x20000000) being set and returns
// -ENODEV otherwise; a GPU whose firmware is GSP-RM never sets it, which is why
// gp100_therm_new() refuses to attach at all under GSP. Here the range is the judge instead:
// a value outside 1..125 C is treated as "no reading" rather than published.
static constexpr UInt32   kNvTsensOffset     = 0x20460;
static constexpr UInt32   kNvTsensValueMask  = 0x0001fff8u;
static constexpr unsigned kNvTempMinC        = 1;
static constexpr unsigned kNvTempMaxC        = 125;
// The family rewrites PerformanceStatistics 0.25-2 s apart, so a slower writer would be
// overwritten before iStat reads it; 250 ms keeps our key present essentially always.
static constexpr unsigned kNvTempIntervalMs  = 250;
// Nothing touches BAR0 until the GPU has settled: bring-up still runs for a while after start().
static constexpr unsigned kNvTempFirstMs     = 5000;
static constexpr unsigned kNvUtilIntervalMs  = 1000;
#ifndef NMX_ACCEL   // read only by the NVIDIA build's utilisation publisher
static constexpr uint64_t kNvUtilMaxAgeNs    = 5000000000ULL;
#endif

extern "C" {   // kern/thread_call.h and kern/clock.h clash with the family headers here, so the KPIs are declared by hand
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter_delayed(struct thread_call *call, uint64_t deadline);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);
uint64_t mach_absolute_time(void);
uint64_t mach_continuous_time(void);
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
void proc_selfname(char *buf, int size);
}
static volatile SInt64 gCntVmAlloc, gCntVmAllocBytes, gCntVmDealloc, gCntVmDeallocBytes, gCntVmRefused,
                       gCntResNew, gCntResFree, gCntMapCommit, gCntMapRelease, gCntMapFree;
static void nvaccelInstallSysctl(void);
static int gNvAccelArmed = 0;
// 1 once the headless check restored the plugin name without arming (no NVIDIA display head). The Metal plugin reads it:
// a headless GPU hands frames to another GPU's WindowServer through system memory, so IOSurfaces must be copied out.
static int gNvHeadless = 0;
static unsigned gNmFlatSkip = 0;   // boot-arg nmflatskip=<n>: frames of one flat colour held back, at most n in a row (0 = off)
static SInt64 gCntFlatHeld;
static struct nmh_flat gFlat[5];
static int gNmHybridP2P = 0;   // boot-arg nmhybridp2p (nm_hybrid_p2p.h): 0 off, 1 known-good host bridges, 2 forced; default off
static int gNmHybrid = 0;   // boot-arg nmhybrid (nm_hybrid.h): 0 off, 1 copy, 2 import; default off
#ifdef NMX_ACCEL
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = AMD/Intel Metal device for apps; frames are copied to the framebuffer WindowServer draws");
#else
SYSCTL_INT(_debug, OID_AUTO, nvaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = no NVIDIA display: Metal device for apps only, WindowServer on the GPU that owns the display");
#endif
// 4 NVIDIA heads (nvfbheads=4) + 1 for a foreign hybrid built-in panel (NMIntelFB with independent Intel controller
// local index 0, or legacy NMGopFB, routed to logical slot 4): WindowServer composites that panel on the NVIDIA card
// and the display-pipe copy path below writes each frame into the panel's scan-out surface.
static const unsigned kNvMaxFB = NVRM_FB_MAX_HEADS;
static_assert(NVRM_FB_MAX_HEADS == 5, "NVRM_FB_MAX_HEADS changed: keep kNvMaxFB in sync");
static unsigned gNvHeadsPublished = 0, gNvHeadsRegistered = 0;
static unsigned gNvHeadsPiped = 0;
// The Intel built-in panel (head 4) joins only when this is 1. Off by default while the path is proven: a write at run
// time (then debug.nvaccelfb=3 and a WindowServer restart) can be undone by a reboot, so a hang never needs hands
// (the first virtual-panel test froze the machine at boot and needed a power cycle).
static int gNvIgpu = 0;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_igpu, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvIgpu, 0,
           "1 = give an Intel built-in panel (NMIntelFB, head 4) a display pipe on this accelerator");

extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMX_ACCEL
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMXAccel, "0.1", _start, _stop) }
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVAccel, "0.1", _start, _stop) }
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

class NVEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(NVEventMachine)
    NM_TAHOE_FWD(NVEventMachine)
public:
    void writeStamp(int s, vendevtCommandRec *r, unsigned int v) APPLE_KEXT_OVERRIDE {
        (void)r;
        nvAccelWriteStamp(s, v);
    }
    void prepareBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOGV("EM prepareBarrier(%p)", b); }
    void completeBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOGV("EM completeBarrier(%p)", b); }
    void writeBarrierElement(vendevtBarrierRec *b, int i, unsigned int v) APPLE_KEXT_OVERRIDE { ALOGV("EM writeBarrierElement(%p, %d, %u)", b, i, v); }
    bool checkGPUProgress() APPLE_KEXT_OVERRIDE { return true; }
    bool checkChannelProgress(int ch) APPLE_KEXT_OVERRIDE { return true; }
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE { ALOGV("EM eventTimeout(%d) — ignored, we do not use family channels", stamp); return nullptr; }
};
OSDefineMetaClassAndStructors(NVEventMachine, IOAccelEventMachineFast2)
static void (*gEnableAccel)(const char *why) = nullptr;
static int gNvScanout = 0;
static int gNvAperture = 0;
static int gNvIop = 0;
class NVDisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(NVDisplayMachine)
    NM_TAHOE_FWD(NVDisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { ALOG("DM displayModeWillChange"); return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        ALOG("DM displayModeDidChange -> true");
        if (gEnableAccel) gEnableAccel("displayModeDidChange");
        return true;
    }
};
OSDefineMetaClassAndStructors(NVDisplayMachine, IOAccelLegacyDisplayMachine)
#define NV_PRIVATE_VA_BASE  0x800000000000ull
#define NV_PRIVATE_VA_SIZE  0x000010000000ull
class NVTask : public IOAccelTask {
    OSDeclareDefaultStructors(NVTask)
    NM_TAHOE_FWD(NVTask)
public:
    IORangeAllocator *fVa = nullptr;
    unsigned fAllocLogged = 0, fPrivate = 0;
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE {
        const unsigned long long len = map ? ((IOAccelMemoryMap *)map)->getLength() : 0;
        const unsigned long long align = map ? *(const volatile unsigned long long *)((const unsigned char *)map + 0x80) : 0;
        unsigned long long got = nvAccelGpuVaAlloc((IOAccelMemoryMap *)map, len, align);
        if (!got && fAllocLogged < 12)
            ALOG("NVTask::allocate: RM refused %llu bytes (align %llu) -- no GPU address", len, align);
        return got;
    }
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE {
        (void)addr;
        nvAccelGpuVaFree((IOAccelMemoryMap *)map);
    }
};
OSDefineMetaClassAndStructors(NVTask, IOAccelTask)
class NVMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(NVMemoryMap)
    NM_TAHOE_FWD(NVMemoryMap)
public:
    IOAccelMemory *fMem = nullptr; // borrowed from the base map; changed only after successful base redirect
    NVGpuVaRecord *fRecord = nullptr;
    NVVramOwner *fOwner = nullptr;
    NVAccel *fAccel = nullptr;
    bool init(IOGraphicsAccelerator2 *, IOAccelTask *, IOAccelMemory *, unsigned int) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool redirect(IOAccelMemory *) APPLE_KEXT_OVERRIDE;
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE;
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE;
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return fRecord && fRecord->fMapped; }
};
OSDefineMetaClassAndStructors(NVMemoryMap, IOAccelMemoryMap)

#define NV_CARRIERS 32
class IOAccelNVRMCarrier : public IOService {
    OSDeclareDefaultStructors(IOAccelNVRMCarrier)
};
OSDefineMetaClassAndStructors(IOAccelNVRMCarrier, IOService)

// More than one accelerated card is opt-in (boot argument nvmultigpu=1). Without it the table below behaves as the single
// static pointer it replaces: every start sets it, nothing clears it (nvrm_devtab.h).
static bool nvaccelMultiGpu(void)
{
    static int on = -1;
    if (on < 0) { uint32_t v = 0; on = (PE_parse_boot_argn("nvmultigpu", &v, sizeof v) && v) ? 1 : 0; }
    return on == 1;
}
class NVAccel;
struct NVAccelStartScope {   // releases the start gate on every way out of start()
    bool multi; bool started;
    ~NVAccelStartScope();
};
class NVAccel : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(NVAccel)
    NM_TAHOE_FWD(NVAccel)
    IOAccelNVRMCarrier *fCarriers[NV_CARRIERS] = { nullptr };
    unsigned            fCarrierNext = 0;
    unsigned            fCarrierLogged = 0;
public:
    IOBufferMemoryDescriptor *fStamps = nullptr; IORangeAllocator *fRanges[32] = {};
    uint64_t  getStampMemory(unsigned int *p) APPLE_KEXT_OVERRIDE {
        if (!fStamps) { fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 16384, 4096); if (fStamps) { fStamps->prepare(); bzero(fStamps->getBytesNoCopy(), 16384); } }
        if (p) *p = fStamps ? 4096 : 0;
        ALOGV("getStampMemory(%p) -> descriptor %p (%u stamps, 16 KB)", p, fStamps, p ? *p : 0); return (uint64_t)fStamps; }
    OSObject *newEventMachine() APPLE_KEXT_OVERRIDE { NVEventMachine *m = OSTypeAlloc(NVEventMachine); ALOG("newEventMachine -> %p (family inits it)", m); return m; }
    OSObject *newGPUTask(const char *who) {
        for (unsigned i = 0; i < 32; i++) if (!fRanges[i]) {
            fRanges[i] = IORangeAllocator::withRange(1ULL << 40, 4096);
            if (fRanges[i]) fRanges[i]->allocateRange(0, 0x100000);
        }
        NVTask *t = OSTypeAlloc(NVTask); bool ok = t && t->init(this, 1, fRanges);
        ALOG("%s -> %p init=%d", who, t, ok); if (t && !ok) { t->release(); t = nullptr; } return t; }
    OSObject *createUserGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createUserGPUTask"); }
    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p) { ALOG("populateAccelConfig(NULL) — nothing to do"); return; }
        if (((uintptr_t)p & 0xFFF) + 0x70 > 0x1000) {
            ALOG("populateAccelConfig(%p) — 0x70 bytes would cross out of this page; REFUSING to touch it", p);
            return;
        }
        ALOG("populateAccelConfig(%p) — what the FAMILY pre-filled, before we write a byte:", p);
        for (unsigned i = 0; i < 0x70; i += 8)
            ALOG("   cfg+0x%02x = 0x%016llx", i, *(const uint64_t *)(p + i));

        *(uint64_t *)(p + 0x00) = (uint64_t)(uintptr_t)"NVIDIAAccelerator";
        *(uint32_t *)(p + 0x08) = 0x30844300u;
        *(uint32_t *)(p + 0x0c) = 15u;
        *(uint32_t *)(p + 0x18) = 8u;
        *(uint32_t *)(p + 0x30) = 0x40004000u;
        *(uint32_t *)(p + 0x34) = 0x00008000u;
        *(uint32_t *)(p + 0x38) = 0x00008000u;
        *(uint8_t  *)(p + 0x44) = 0;
        *(uint8_t  *)(p + 0x46) = 1;
        *(uint8_t  *)(p + 0x47) = 1;
        *(uint64_t *)(p + 0x48) = 0x0000001F0000000FULL;
        *(uint64_t *)(p + 0x50) = 0x7FFFFFFF0000001FULL;
        *(uint64_t *)(p + 0x58) = 0x0000000C00000000ULL;
        *(uint32_t *)(p + 0x64) = 0x4Bu;
        *(uint32_t *)(p + 0x28) = kNvMaxFB;
        ALOG("populateAccelConfig(%p) — filled: AccelCaps=15, flags=0x30844300, maxtex=16384x16384, maxfb=%u written (cfg+0x2c left %u)",
             p, kNvMaxFB, *(const uint32_t *)(p + 0x2c));
    }
    bool      configureDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("configureDevice(%p) %04x:%04x — RM owns the card (NVRM.kext); nothing to do here yet", pci, pci ? pci->configRead16(0) : 0, pci ? pci->configRead16(2) : 0); return true; }
    void      teardownDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("teardownDevice(%p)", pci); }
    OSObject *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createKernelGPUTask"); }
    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE {
        NVDisplayPipe *p = OSTypeAlloc(NVDisplayPipe);
        if (!p) { ALOG("newDisplayPipe: OSTypeAlloc FAILED - the family will store this NULL unchecked"); return NULL; }
        p->fAccel = this;
        p->fTransactionLock = IOLockAlloc();
        if (!p->fTransactionLock) { p->release(); return nullptr; }
        ALOG("newDisplayPipe -> %p", p); return p; }
    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE { NVDisplayMachine *d = OSTypeAlloc(NVDisplayMachine);
        fDM = d;
        ALOGV("newDisplayMachine -> %p", d); return d; }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE { NVGLContext *c = OSTypeAlloc(NVGLContext); ALOGV("newGLContext -> %p", c); return c; }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE { NVCLContext *c = OSTypeAlloc(NVCLContext); ALOGV("newCLContext -> %p", c); return c; }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE { NVSurface *o = OSTypeAlloc(NVSurface); ALOGV("newSurface -> %p", o); return o; }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE { NV2DContext *c = OSTypeAlloc(NV2DContext); ALOGV("new2DContext -> %p", c); return c; }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE { NVVideoContext *c = OSTypeAlloc(NVVideoContext); ALOGV("newVideoContext -> %p", c); return c; }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE { NVSysMemory *m = OSTypeAlloc(NVSysMemory); ALOGV("newSysMemory -> %p", m); return m; }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE {
        NVVidMemory *m = OSTypeAlloc(NVVidMemory);
        if (m && !m->bindOwner(fOwner)) { m->release(); m = nullptr; }
        if (m) m->fAccel = this;
        return m;
    }
    OSObject *newResource() APPLE_KEXT_OVERRIDE { NVResource *r = OSTypeAlloc(NVResource); if (r) r->fAccel = this; OSAddAtomic64(1, &gCntResNew); ALOGV("newResource -> %p", r); return r; }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE { NVMemoryMap *m = OSTypeAlloc(NVMemoryMap); if (m) m->fAccel = this; ALOGV("newMemoryMap -> %p", m); return m; }

    NVDisplayMachine *fDM = nullptr;
    IOPCIDevice *fPCI = nullptr;
    // GPU core temperature: BAR0 is mapped lazily on the first tick, never in start().
    IOMemoryMap        *fTempMap    = nullptr;
    volatile UInt32    *fTempRegs   = nullptr;
    IOTimerEventSource *fTempTimer  = nullptr;
    int                 fTempC      = 0;
    UInt32              fTempFails  = 0;
    bool                fTempLogged = false;
    // RM queries can sleep waiting for firmware. Keep them off the graphics work loop;
    // the temperature timer only copies the most recent result into the registry.
    IOWorkLoop         *fUtilLoop = nullptr;
    IOTimerEventSource *fUtilTimer = nullptr;
    IOService          *fUtilProvider = nullptr;
    IOLock             *fUtilLock = nullptr;
    NVRMUtilization     fUtil = {};
    uint64_t            fUtilUpdated = 0;
    bool                fUtilValid = false;
    IONotifier *fFBNote = nullptr;
    IONotifier *fIntelNote = nullptr;
    IONotifier *fGopNote = nullptr;     // NMGopFB: an AMD integrated GPU's panel, published as head 4 with the hybrid switch on
    static IOLock *gAccelLock;
    static NvrmDevTab<NVAccel> gAccelTab;
    static NvrmStartGate gAccelGate;
    static NvrmWaitQueue<IOService> gAccelWait;
    static void publishPrimary() {
        if (!nvaccelMultiGpu() || !gAccelLock) return;
        NVAccel *snapshot[NVRM_DEVTAB_MAX] = {}, *primary = nullptr;
        IOLockLock(gAccelLock);
        primary = gAccelTab.primary();
        for (unsigned i = 0; i < gAccelTab.count(); ++i) {
            snapshot[i] = gAccelTab.at(i); snapshot[i]->retain();
        }
        IOLockUnlock(gAccelLock);
        for (unsigned i = 0; i < NVRM_DEVTAB_MAX; ++i) if (snapshot[i]) {
            snapshot[i]->setProperty("NVAccelPrimary", snapshot[i] == primary ? kOSBooleanTrue : kOSBooleanFalse);
            snapshot[i]->release();
        }
    }
    static void reofferNext() { if (IOService *p = gAccelWait.next()) { p->registerService(kIOServiceAsynchronous); p->release(); } }
    friend struct NVAccelStartScope;
    // Which thread holds the accelerator lock, kept from the family's own did-lock / will-unlock calls (nvFamLock and
    // nvFamUnlock call them too). A page-off lets go of the lock for its GPU wait only when the current thread is the
    // recorded owner at one level (debug.nvaccel_pageoff_unlocked_wait).
    nvaccel_lock_owner fLockOwner;   // per accelerator: each card has its own accelerator lock
    bool mayRelease() { return nvaccel_owner_may_release(&fLockOwner, current_thread()); }
    bool familyOwned() const { return __atomic_load_n(&fLockOwner.owner, __ATOMIC_ACQUIRE) == current_thread() && __atomic_load_n(&fLockOwner.depth, __ATOMIC_RELAXED) > 0; }
    void acceleratorDidLock(char const *f, int l) APPLE_KEXT_OVERRIDE { IOGraphicsAccelerator2::acceleratorDidLock(f, l); nvaccel_owner_did_lock(&fLockOwner, current_thread()); }
    void acceleratorWillUnlock(char const *f, int l) APPLE_KEXT_OVERRIDE { nvaccel_owner_will_unlock(&fLockOwner, current_thread()); IOGraphicsAccelerator2::acceleratorWillUnlock(f, l); }
    static NVAccel *resolve(NVAccel *owner) { return owner; }
    // Callers adopt the returned reference before dropping it; stop removes the entry first.
    static NVAccel *forDisplay() {
        if (!gAccelLock) return nullptr;
        IOLockLock(gAccelLock);
        NVAccel *a = gAccelTab.primary();
        if (a && !a->fStopping && a->fOwner && a->fOwner->enter()) a->retain(); else a = nullptr;
        IOLockUnlock(gAccelLock);
        return a;
    }
    NVVramOwner *fOwner = nullptr;
    bool fCandidate = false, fStopping = false, fTargetRegistered = false;
    IOService *locateNVRM();
    IOService *fFB = nullptr;
    IOService *volatile fFBs[kNvMaxFB] = {};
    unsigned fFBRegistered = 0;
    IOLock *fRegLock = nullptr;
    OSObject *fMetalPlugin = nullptr;
    unsigned fHead4AssocLogs = 0;
    bool fHead4AssocActive = false;
    uint64_t fHead4AssocRegId = 0;
    uint32_t fHead4AssocDense = 0;
    static enum nvrm_fb_kind classifyFramebuffer(IOService *fb) {
        if (!fb) return NVRM_FB_UNKNOWN;
        if (fb->metaCast("NVRMNVDAFramebuffer")) return NVRM_FB_NVIDIA;
        if (fb->metaCast("NMIntelFB"))           return NVRM_FB_INTEL;
        if (fb->metaCast("NMGopFB"))             return NVRM_FB_GOP;
        return NVRM_FB_UNKNOWN;
    }
    static void readFbTopologyProps(IOService *fb, int *hasId, uint64_t *depId, int *hasIdx, uint32_t *localIdx) {
        *hasId = 0; *depId = 0; *hasIdx = 0; *localIdx = 0;
        if (!fb) return;
        OSObject *idObj = fb->copyProperty("IOFBDependentID");
        OSObject *idxObj = fb->copyProperty("IOFBDependentIndex");
        OSNumber *idNum = OSDynamicCast(OSNumber, idObj);
        OSNumber *idxNum = OSDynamicCast(OSNumber, idxObj);
        if (idNum) { *hasId = 1; *depId = (uint64_t)idNum->unsigned64BitValue(); }
        if (idxNum) { *hasIdx = 1; *localIdx = (uint32_t)idxNum->unsigned32BitValue(); }
        OSSafeReleaseNULL(idObj);
        OSSafeReleaseNULL(idxObj);
    }
    void clearHead4AssociationLocked() {
        if (fHead4AssocActive || getProperty("NVAccelHead4RegistryID") || getProperty("NVAccelHead4DensePipeIndex")) {
            removeProperty("NVAccelHead4RegistryID");
            removeProperty("NVAccelHead4DensePipeIndex");
            fHead4AssocActive = false;
            fHead4AssocRegId = 0;
            fHead4AssocDense = 0;
        }
    }
    void logHead4AssociationBounded(IOService *fb, unsigned denseIdx, bool matched) {
        if (fHead4AssocLogs >= 8) return;
        fHead4AssocLogs++;
        int hasId = 0, hasIdx = 0;
        uint64_t depId = 0; uint32_t localIdx = 0;
        readFbTopologyProps(fb, &hasId, &depId, &hasIdx, &localIdx);
        ALOG("hybrid: head4 association class=%s depID=0x%llx (has=%d) localIdx=%u (has=%d) logical=%d dense=%u match=%d",
             fb ? fb->getMetaClass()->getClassName() : "(null)",
             (unsigned long long)depId, hasId, localIdx, hasIdx,
             NVRM_FB_LOGICAL_HYBRID_HEAD, denseIdx, matched ? 1 : 0);
    }
    bool findVerifiedHead4PipeLocked(IOService *fb, unsigned *outDense) {
        if (!fb || !fDM || fb != fFBs[NVRM_FB_LOGICAL_HYBRID_HEAD] || fb->isInactive()) return false;
        if (!(gNvHeadsPiped & NVRM_FB_HYBRID_PUBLISHED_BIT)) return false;
        uint8_t *dm = (uint8_t *)fDM;
        const unsigned count = fDM->getFramebufferCount();
        if (!count || count > 16) return false;
        unsigned matches = 0, foundIdx = 0;
        for (unsigned d = 0; d < count; d++) {
            void *pipe = *(void *const volatile *)(dm + 0x88 + 8 * d);
            if (!pipe) continue;
            IOService *pfb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
            if (pfb == fb) { foundIdx = d; matches++; }
        }
        if (matches != 1) return false;
        if (!nm_optc_head4_assoc_valid(fFBs[NVRM_FB_LOGICAL_HYBRID_HEAD], fb, foundIdx, count, fb->isInactive() ? 1 : 0))
            return false;
        if (outDense) *outDense = foundIdx;
        return true;
    }
    void publishHead4AssociationLocked(IOService *fb, unsigned denseIdx) {
        if (!fb || !fDM || fb != fFBs[NVRM_FB_LOGICAL_HYBRID_HEAD]) {
            clearHead4AssociationLocked();
            return;
        }
        const unsigned count = fDM->getFramebufferCount();
        void *pipe = (denseIdx < count && denseIdx < 16) ? *(void *const volatile *)((const uint8_t *)fDM + 0x88 + 8 * denseIdx) : nullptr;
        IOService *pfb = pipe ? *(IOService *const volatile *)((const uint8_t *)pipe + 0x98) : nullptr;
        if (!nm_optc_head4_assoc_valid(fFBs[NVRM_FB_LOGICAL_HYBRID_HEAD], pfb, denseIdx, count, fb->isInactive() ? 1 : 0)) {
            clearHead4AssociationLocked();
            logHead4AssociationBounded(fb, denseIdx, false);
            return;
        }
        int hasId = 0, hasIdx = 0; uint64_t depId = 0; uint32_t localIdx = 0;
        readFbTopologyProps(fb, &hasId, &depId, &hasIdx, &localIdx);
        if (nvrm_fb_logical_head(classifyFramebuffer(fb), hasId, depId, hasIdx, localIdx) != NVRM_FB_LOGICAL_HYBRID_HEAD) {
            clearHead4AssociationLocked();
            logHead4AssociationBounded(fb, denseIdx, false);
            return;
        }
        const uint64_t regId = fb->getRegistryEntryID();
        if (!regId ||
            !setProperty("NVAccelHead4RegistryID", (unsigned long long)regId, 64) ||
            !setProperty("NVAccelHead4DensePipeIndex", (unsigned long long)denseIdx, 32)) {
            clearHead4AssociationLocked();
            ALOG("hybrid: head4 association property publish failed (dense=%u)", denseIdx);
            return;
        }
        const bool changed = !fHead4AssocActive || fHead4AssocRegId != regId || fHead4AssocDense != denseIdx;
        fHead4AssocActive = true;
        fHead4AssocRegId = regId;
        fHead4AssocDense = denseIdx;
        if (changed) logHead4AssociationBounded(fb, denseIdx, true);
    }
    void syncHead4AssociationLocked() {
        IOService *fb4 = fFBs[NVRM_FB_LOGICAL_HYBRID_HEAD];
        unsigned denseIdx = 0;
        if (findVerifiedHead4PipeLocked(fb4, &denseIdx)) {
            publishHead4AssociationLocked(fb4, denseIdx);
        } else {
            clearHead4AssociationLocked();
        }
    }
    bool hasActiveHead() const {
        return nm_optc_has_active_head(fFB != nullptr ? 1 : 0, gNmHybrid, gNvIgpu, fFBs[4] != nullptr ? 1 : 0) != 0;
    }
    IOReturn registerTarget(bool add) {
        if (!fOwner) return kIOReturnNotReady;
        NVRMAccelTarget q = { NVRM_OWNER_ABI_VERSION, sizeof(q),
            add ? NVRM_ACCEL_TARGET_REGISTER : NVRM_ACCEL_TARGET_UNREGISTER, 0, this, fOwner->fState.generation };
        IOReturn r = fOwner->call(NVRM_ACCEL_TARGET_FN, &q);
        if (r == kIOReturnSuccess) fTargetRegistered = add;
        return r;
    }
    static bool fbPublished(void *target, void *ref, IOService *newService, IONotifier *note) {
        (void)ref; (void)note;
        NVAccel *me = (NVAccel *)target;
        if (!me || !newService || __atomic_load_n(&me->fStopping, __ATOMIC_ACQUIRE)) return true;
        const enum nvrm_fb_kind kind = classifyFramebuffer(newService);
        int hasId = 0, hasIdx = 0;
        uint64_t depId = 0; uint32_t localIdx = 0;
        readFbTopologyProps(newService, &hasId, &depId, &hasIdx, &localIdx);
        const int logical = nvrm_fb_logical_head(kind, hasId, depId, hasIdx, localIdx);
        if (logical < 0 || (unsigned)logical >= kNvMaxFB) {
            ALOG("%s rejected -- class %s depID=0x%llx (has=%d) localIdx=%u (has=%d) invalid for logical topology",
                 newService->getName(), newService->getMetaClass()->getClassName(),
                 (unsigned long long)depId, hasId, localIdx, hasIdx);
            return true;
        }
        const unsigned slot = (unsigned)logical;
        if (kind == NVRM_FB_NVIDIA && (!me->fOwner || !me->fOwner->bindDisplay(newService, slot))) return true;
        const char *what = "kept";
        bool validOwner = false;
        enum nm_optc_claim_outcome outcome = nm_optc_decide_claim(me->fFBs[slot], newService);
        if (outcome == NM_OPTC_CLAIM_IDEMPOTENT) {
            OSBitOrAtomic(1u << slot, &gNvHeadsPublished);
            what = "idempotent";
            validOwner = true;
        } else if (outcome == NM_OPTC_CLAIM_ACQUIRED) {
            newService->retain();
            if (OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFBs[slot])) {
                OSBitOrAtomic(1u << slot, &gNvHeadsPublished);
                validOwner = true;
            } else {
                newService->release();
                outcome = nm_optc_decide_claim(me->fFBs[slot], newService);
                if (outcome == NM_OPTC_CLAIM_IDEMPOTENT) {
                    OSBitOrAtomic(1u << slot, &gNvHeadsPublished);
                    what = "idempotent";
                    validOwner = true;
                } else {
                    what = "CONFLICT -- slot already claimed, rejecting";
                }
            }
        } else {
            what = "CONFLICT -- slot already claimed, rejecting";
        }
        if (!validOwner) {
            ALOG("%s (%s) local %u -> logical %u %s (depID 0x%llx)",
                 newService->getName(), newService->getMetaClass()->getClassName(),
                 localIdx, slot, what, (unsigned long long)depId);
            return true;
        }
        // Only NVRMNVDAFramebuffer at logical slot 0 may become the primary NVIDIA framebuffer (fFB).
        if (nm_optc_should_set_primary_fb(kind, logical) && me->fFBs[0] == newService && me->fFB != newService) {
            newService->retain();
            if (!OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFB)) {
                newService->release();
            }
        }
        unsigned before = me->fDM ? me->fDM->getFramebufferCount() : 0xFFFFFFFFu;
        ALOG("%s (%s) published -- display machine framebuffer count = %u (depID 0x%llx local %u -> logical %u %s)",
             newService->getName(), newService->getMetaClass()->getClassName(), before,
             (unsigned long long)depId, localIdx, slot, what);
        if (me->fCandidate) { me->registerFramebuffer(1); return true; }
        if (gNvAccelArmed)
            ALOG("  logical %u published AFTER the arm: it gets a pipe only when debug.nvaccelfb=3 is written again, "
                 "and WindowServer composites it only after a WindowServer restart", slot);
        else
            ALOG("  to register it:  sudo sysctl -w debug.nvaccelfb=1   (2 = re-run the family's own sweep)");
        return true;
    }
    // Headless GPU (laptops whose panel and outputs are all on the Intel or AMD GPU: issues #76, #106). NVKMS finds no
    // display, so no NVRMFB head ever publishes and the withheld MetalPluginName was never restored: Metal never offered
    // the card, although NVRM and NVK run it fine (#106: vkCreateInstance enumerates the GTX 1660 Ti). Apple's own
    // two-GPU MacBook Pros work this way: the integrated GPU drives every display and WindowServer, and the discrete GPU
    // is a second Metal device that apps choose and macOS copies their frames from. So when no head has published by the
    // deadline, the plugin name goes back on WITHOUT arming: apps get the NVIDIA device, and WindowServer (allow-listed
    // "!WindowServer" = only when armed) keeps the GPU that owns the display. Nothing here touches a display pipe or a
    // mode change. Boot-arg -nvaccelnoheadless keeps the old behaviour.
    // The check runs every few seconds rather than once at the deadline: on a laptop whose screen is on the other GPU,
    // NVKMS has listed this card's connectors, with none connected, about 7 s after start, and every app opened before
    // the plugin name comes back never sees the NVIDIA device (Ampere and Turing laptops reported "Metal device: no" at
    // first login while the name came back at 60 s). So the name goes back as soon as both are true: NVKMS reports no
    // display on any of this card's connectors, and another GPU's framebuffer already shows the desktop. A desktop whose
    // monitor is slow to wake has no other GPU's framebuffer and still waits for the deadline.
    static constexpr uint32_t kHeadlessDeadlineSec = 60;   // chosen: NVRMFB heads publish within ~20 s of auto-go's bring-up (#76 log)
    // base wait counted from the end of auto-go's settle delay, not from this accelerator's start
    uint32_t fHeadlessDeadlineSec = kHeadlessDeadlineSec;
    static constexpr uint32_t kHeadlessPollSec = 2;        // chosen: a small fraction of the ~7 s NVKMS takes to enumerate
    struct thread_call *fHeadlessCall = nullptr;
    volatile UInt32 fHeadlessArmed = 0;
    uint32_t fHeadlessWaited = 0;
    // true when NVKMS enumerated this card's connectors (NVRMFB writes connected-displays on each display nub it
    // enumerates) and found a display on none of them
    static bool headlessNoDisplayHere() {
        bool seen = false, none = true;
        if (OSIterator *it = getMatchingServices(serviceMatching("NVRMDisplay"))) {
            while (OSObject *o = it->getNextObject()) {
                IOService *s = OSDynamicCast(IOService, o);
                OSNumber *cd = s ? OSDynamicCast(OSNumber, s->getProperty("connected-displays")) : nullptr;
                if (!cd) continue;
                seen = true;
                if (cd->unsigned32BitValue() > 0) none = false;
            }
            it->release();
        }
        return seen && none;
    }
    // true when an accelerator that names a Metal plugin sits under this PCI device (two levels covers the accelerators
    // macOS's own GPU drivers publish: the accelerator is a child of the PCI device or of its framebuffer controller)
    static bool gpuHasMetalAccelerator(IORegistryEntry *pci) {
        if (!pci) return false;
        bool found = false;
        if (OSIterator *it = pci->getChildIterator(gIOServicePlane)) {
            while (!found) {
                IORegistryEntry *c = OSDynamicCast(IORegistryEntry, it->getNextObject());
                if (!c) break;
                if (c->getProperty("MetalPluginName")) { found = true; break; }
                if (OSIterator *it2 = c->getChildIterator(gIOServicePlane)) {
                    while (IORegistryEntry *g = OSDynamicCast(IORegistryEntry, it2->getNextObject()))
                        if (g->getProperty("MetalPluginName")) { found = true; break; }
                    it2->release();
                }
            }
            it->release();
        }
        return found;
    }
    // the first framebuffer that is not one of this driver's heads; *other is set when it sits on another vendor's GPU
    // that can give WindowServer a Metal device
    static void headlessDisplayOwner(char *owner, size_t len, bool *other) {
        *other = false;
        OSIterator *it = getMatchingServices(serviceMatching("IOFramebuffer"));
        if (!it) return;
        while (OSObject *o = it->getNextObject()) {
            IOService *s = OSDynamicCast(IOService, o);
            if (!s || s->metaCast("NVRMNVDAFramebuffer")) continue;
            strlcpy(owner, s->getMetaClass()->getClassName(), len);
            // the Intel panel driver publishes as a head of this accelerator, so it is not another GPU's desktop here
            if (s->metaCast("NMIntelFB")) break;
            // the same for the firmware-framebuffer panel provider (an AMD integrated GPU's panel), with the switch on
            if (gNmHybrid && s->metaCast("NMGopFB")) break;
            // with the hybrid switch on, an AMD or Intel panel driven by NMXGPU is also a head of this accelerator
            if (gNmHybrid && s->metaCast("NMXGPUFB")) break;
            IORegistryEntry *e = s->getProvider();
            for (int up = 0; e && up < 8 && !e->metaCast("IOPCIDevice"); up++) e = e->getParentEntry(gIOServicePlane);
            OSData *v = e ? OSDynamicCast(OSData, e->getProperty("vendor-id")) : nullptr;
            if (v && v->getLength() >= 2) {
                const uint8_t *b = (const uint8_t *)v->getBytesNoCopy();
                *other = (uint16_t)(b[0] | (b[1] << 8)) != 0x10de;
            }
            // Another GPU only takes the desktop if it can give WindowServer a Metal device. A framebuffer with no
            // Metal accelerator on its GPU (IONDRVFramebuffer on an integrated GPU macOS 26 no longer accelerates) cannot:
            // treating it as the desktop's GPU made the plugin refuse WindowServer, and with no Metal device anywhere
            // WindowServer aborted at start ("Failed to create MetalDevice") every ten seconds, a black screen with a cursor.
            if (*other && !gpuHasMetalAccelerator(e)) {
                *other = false;
                strlcat(owner, " without a Metal accelerator", len);
            }
            break;
        }
        it->release();
    }
    void armHeadlessCheck() {
        int off = 0;
        if (PE_parse_boot_argn("-nvaccelnoheadless", &off, sizeof off)) { ALOG("headless check: off (-nvaccelnoheadless)"); return; }
        fHeadlessCall = thread_call_allocate(&NVAccel::headlessCheck, this);
        if (!fHeadlessCall) { ALOG("headless check: thread_call_allocate FAILED -- a GPU with no display stays hidden from Metal"); return; }
        fHeadlessWaited = 0;
        {   // auto-go publishes its settle delay on the PCI device; heads cannot exist before it has passed
            uint32_t settleMs = 0;
            if (IORegistryEntry *pci = getProvider())
                if (OSNumber *n = OSDynamicCast(OSNumber, pci->getProperty("nvrm-autogo-settle-ms"))) settleMs = n->unsigned32BitValue();
            fHeadlessDeadlineSec = nvrmHeadlessDeadlineSec(kHeadlessDeadlineSec, settleMs);
        }
        uint64_t when = 0;
        clock_interval_to_deadline(kHeadlessPollSec, kSecondScale, &when);
        __atomic_store_n(&fHeadlessArmed, 1, __ATOMIC_RELEASE);
        thread_call_enter_delayed(fHeadlessCall, when);
        ALOG("headless check: armed, every %u s for up to %u s", kHeadlessPollSec, fHeadlessDeadlineSec);
    }
    static void headlessCheck(void *p0, void *) {
        NVAccel *me = (NVAccel *)p0;
        if (!me) return;
        bool rearmed = false;
        struct ScopedArmClear {
            volatile UInt32 *armed;
            bool *keep;
            ~ScopedArmClear() { if (!*keep) __atomic_store_n(armed, 0, __ATOMIC_RELEASE); }
        } armClear { &me->fHeadlessArmed, &rearmed };
        if (__atomic_load_n(&me->fStopping, __ATOMIC_ACQUIRE)) return;
        if (me->fRegLock) IOLockLock(me->fRegLock);
        const char *why = me->hasActiveHead() ? "a display head published" : !me->fMetalPlugin ? "no plugin name to restore"
                        : me->getProperty("MetalPluginName") ? "the plugin name is already back" : nullptr;
        me->fHeadlessWaited += kHeadlessPollSec;
        if (why) {
            ALOG("headless check: not headless (%s) -- nothing to do", why);
        } else {
            // name the GPU that owns the display, so a log says which driver WindowServer is on
            char owner[64] = "none found";
            bool other = false;
            headlessDisplayOwner(owner, sizeof owner, &other);
            const bool early = other && headlessNoDisplayHere();
            if (!early && me->fHeadlessWaited < me->fHeadlessDeadlineSec) {
                if (!__atomic_load_n(&me->fStopping, __ATOMIC_ACQUIRE)) {
                    uint64_t when = 0;
                    clock_interval_to_deadline(kHeadlessPollSec, kSecondScale, &when);
                    thread_call_enter_delayed(me->fHeadlessCall, when);
                    rearmed = true;
                }
                if (me->fRegLock) IOLockUnlock(me->fRegLock);
                return;
            }
            // 2.5.3 field logs (desktops whose only GPU is this card): no head by the deadline and no other GPU showing the
            // desktop. With the name back, CoreDisplay asked this GPU's Metal plugin for WindowServer's device, the plugin
            // refuses WindowServer on a headless GPU, and WindowServer aborted at start ("Failed to create MetalDevice")
            // every ten seconds. With no other GPU there is no display to hand frames to, so the name stays withheld:
            // WindowServer runs unaccelerated on the firmware framebuffer and the desktop comes up, as before the check.
            if (!other && strcmp(owner, "NMIntelFB") != 0 && !(gNmHybrid && (strcmp(owner, "NMXGPUFB") == 0 || strcmp(owner, "NMGopFB") == 0))) {
                ALOG("headless check: no NVIDIA display head after %u s and no other GPU shows the desktop (%s) -- "
                     "MetalPluginName stays withheld so WindowServer starts on the firmware framebuffer",
                     me->fHeadlessWaited, owner);
                if (me->fRegLock) IOLockUnlock(me->fRegLock);
                return;
            }
            me->setProperty("MetalPluginName", me->fMetalPlugin);
            me->setProperty("NVAccelHeadless", kOSBooleanTrue);
            gNvHeadless = 1;
            ALOG("headless check: %s after %u s -- MetalPluginName restored WITHOUT arming: apps get "
                 "the NVIDIA Metal device, WindowServer stays on the display's GPU (%s)",
                 early ? "no display on any NVIDIA connector and another GPU shows the desktop" : "no NVIDIA display head, another GPU shows the desktop",
                 me->fHeadlessWaited, owner);
            // Not a good start for the boot guard: a card whose connectors carry no display clears the guard in the
            // framebuffer's own enumeration (nvrm-fb.cpp), with the connector list as evidence. Reaching this deadline
            // without that means the framebuffer never enumerated at all, which is a failure the guard must keep counting.
        }
        if (me->fRegLock) IOLockUnlock(me->fRegLock);
    }
    void deliverModeChange() {
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        ALOG("  delivering display_mode_will_change/did_change (nest %u, pipes %u)",
             *(const volatile uint32_t *)(dm + 0x10c), *(const volatile uint32_t *)(dm + 0x108));
        fDM->display_mode_will_change(0);
        fDM->display_mode_did_change(0);
        reportPipeGates("after deliverModeChange");
    }
    void reportPipeGates(const char *when) {
        // Log-only read of the family's accelerator flags byte. Its offset moves per release: read in the family's
        // unlock_busy on 10.11 to 10.14; not located on 10.15, where the line is skipped instead of guessed.
#if defined(NM_ERA) && NM_ERA == 1015
        const unsigned kGateOff = 0;
#elif defined(NM_ERA) && NM_ERA < 1012
        const unsigned kGateOff = 0xbd0;
#elif defined(NM_ERA) && NM_ERA < 1013
        const unsigned kGateOff = 0xc48;
#elif defined(NM_ERA) && NM_ERA < 1015
        const unsigned kGateOff = 0xc58;
#else
        const unsigned kGateOff = 0xc78;
#endif
        if (kGateOff) {
            uint8_t accelFlags = *(const volatile uint8_t *)((const uint8_t *)this + kGateOff);
            ALOG("  GATES %s: accel+0x%x = 0x%02x (enabled bit 0x2 %s)",
                 when, kGateOff, accelFlags, (accelFlags & 0x2) ? "SET" : "CLEAR <- kIOReturnNotReady");
        }
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        uint32_t n = *(const volatile uint32_t *)(dm + 0x108);
        if (n > 8) { ALOG("  GATES %s: implausible pipe count %u -- not walking it", when, n); return; }
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *p = *(uint8_t **)(dm + 0x88 + 8 * i);
            if (!p) { ALOG("  GATES %s: pipe %u is NULL", when, i); continue; }
            ALOG("  GATES %s: pipe %u active(+0x298) = %u, wsaaDefer(+0x282) = %u",
                 when, i, *(const volatile uint8_t *)(p + 0x298), *(const volatile uint8_t *)(p + 0x282));
        }
    }
    unsigned registerPendingLocked(const char *why) {
        unsigned added = 0;
        for (unsigned i = 0; i < kNvMaxFB; i++) {
            IOService *fb = fFBs[i];
            if (!fb) {
                // The Intel panel (logical slot 4, NMIntelFB at local index 0 on its Intel controller) does not wait for
                // NVIDIA heads that never publish: a machine with one monitor publishes only head 0, and the walk stopped
                // there, so the Intel head got no pipe (measured 10-10, published 0x11 registered 0x1). Pipes are found by
                // their framebuffer pointer, not their number; dense pipe index is distinct from logical slot 4.
                if (gNvIgpu && i < 4 && ((gNvHeadsPublished >> 4) & 1)) { i = 3; continue; }
                if (gNvHeadsPublished >> i)
                    ALOG("  %s: index %u has not published (published 0x%x): stopping, so no head above it gets a pipe "
                         "this walk", why, i, gNvHeadsPublished);
                break;
            }
            if (fFBRegistered & (1u << i)) continue;
            if (i == 4 && !gNvIgpu) break;   // the Intel panel waits for debug.nvaccel_igpu=1
            unsigned b = fDM->getFramebufferCount();
            ALOG("  %s: calling found_framebuffer(%s index %u) -- count before %u", why, fb->getName(), i, b);
            fDM->found_framebuffer((IOFramebuffer *)fb);
            unsigned c = fDM->getFramebufferCount();
            fFBRegistered |= 1u << i;
            gNvHeadsRegistered = fFBRegistered;
            if (c != b + 1) {
                ALOG("  %s: index %u -- count went %u -> %u; the family did not add it (already known, or the cap "
                     "accel+0xcb0 = %u refused it). Stopping here so no later head takes this one's pipe number.",
                     why, i, b, c, *(const volatile uint32_t *)((const uint8_t *)this + 0xcb0));
                if (i == (unsigned)NVRM_FB_LOGICAL_HYBRID_HEAD) clearHead4AssociationLocked();
                break;
            }
            uint8_t *dm = (uint8_t *)fDM;
            void *pipe = (b < 16) ? *(void *const volatile *)(dm + 0x88 + 8 * b) : nullptr;
            if (!pipe) {
                *(volatile uint32_t *)(dm + 0x108) = b;
                ALOG("  %s: index %u -- the family stored a NULL pipe in slot %u (0xe3b5, unchecked) and counted it; "
                     "count put back %u -> %u so no broadcast dereferences it (0xe320). This head gets no pipe this boot.",
                     why, i, b, c, fDM->getFramebufferCount());
                if (i == (unsigned)NVRM_FB_LOGICAL_HYBRID_HEAD) clearHead4AssociationLocked();
                break;
            }
            IOService *pfb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
            if (pfb != fb) {
                ALOG("  %s: index %u -- pipe %u (%p) drives %p, not %s %p; keeping the pipe (it is real), stopping "
                     "the walk so no later head's pipe number is off by this one", why, i, b, pipe, pfb, fb->getName(), fb);
                if (i == (unsigned)NVRM_FB_LOGICAL_HYBRID_HEAD) {
                    clearHead4AssociationLocked();
                    logHead4AssociationBounded(fb, b, false);
                }
                break;
            }
            ALOG("  %s: logical %u -> dense pipe %u (%p), count %u", why, i, b, pipe, c);
            gNvHeadsPiped |= 1u << i;
            if (i == (unsigned)NVRM_FB_LOGICAL_HYBRID_HEAD) {
                publishHead4AssociationLocked(fb, b);
            }
            added++;
        }
        return added;
    }
    void onCountGrewLocked() {
        deliverModeChange();
        gNvAccelArmed = 1;
        if (fMetalPlugin && !getProperty("MetalPluginName")) {
            setProperty("MetalPluginName", fMetalPlugin);
            ALOG("  MetalPluginName restored -- a NEW process that asks Metal for this accelerator "
                 "can now load the plugin (already-running ones will not)");
        }
        if (!getProperty("IOGLBundleName")) {
            setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
            ALOG("  IOGLBundleName=AppleMetalOpenGLRenderer -- NEW GL processes get a hardware renderer over Metal");
        }
        ALOG("  debug.nvaccelfb now reads 1 -- armed-only Metal clients may have a device THIS BOOT ONLY "
             "(heads published 0x%x, registered 0x%x, piped 0x%x)", gNvHeadsPublished, gNvHeadsRegistered, gNvHeadsPiped);
    }
    void registerFramebuffer(int how) {
        if (fRegLock) IOLockLock(fRegLock);
        registerFramebufferLocked(how);
        if (fRegLock) IOLockUnlock(fRegLock);
    }
    bool registerFramebufferLocked(int how) {
        if (!fDM) { ALOG("registerFramebuffer(%d): no display machine", how); return false; }
        if (!hasActiveHead()) { ALOG("registerFramebuffer(%d): no framebuffer has published yet", how); return false; }
        unsigned before = fDM->getFramebufferCount();
        ALOG("registerFramebuffer(%d): framebuffer count before = %u", how, before);
        if (how == 2) {
            if (before != 0) ALOG("  already %u -- refusing to re-run the family's sweep", before);
            else { ALOG("  re-running IOAccelDisplayMachine::start(pci) -- the family's own sweep"); fDM->start(fPCI); }
        } else {
            registerPendingLocked("sysctl");
        }
        unsigned after = fDM->getFramebufferCount();
        ALOG("  framebuffer count after = %u", after);
        syncHead4AssociationLocked();
        if (after > before) { onCountGrewLocked(); return true; }
        return false;
    }
    // Intel never exports a borrowed scanout pointer; its only consumer is presentFinal().
    IOReturn askFramebufferForScanout(struct NVRMVramRequest *r, IOService *fb = nullptr) {
        if (!fb) fb = fFB;
        if (!fb || fStopping || fb->isInactive()) return kIOReturnNotAttached;
        if (fb->metaCast("NMIntelFB")) { if (r) bzero(r, sizeof(*r)); return kIOReturnUnsupported; }
        if (!fSymScanout) fSymScanout = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_SCANOUT);
        return fb->callPlatformFunction(fSymScanout, false, r, nullptr, nullptr, nullptr);
    }
    const OSSymbol *fSymScanout = nullptr;
    IOService *pipeFramebuffer(const void *pipe, unsigned *idx) {
        if (!pipe) return nullptr;
        IOService *fb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
        if (!fb) return nullptr;
        for (unsigned i = 0; i < kNvMaxFB; i++)
            if (fFBs[i] == fb) { if (idx) *idx = i; return fb; }
        return nullptr;
    }
    IOService *fNVRM = nullptr;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE {
        if ((type & 0xFFFF0000u) != 0x4E560000u) {
            IOReturn fr = IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);
            if (fUcLogged < 40) { fUcLogged++;
                ALOGV("newUserClient(type %u / 0x%x) -> family returned 0x%x, client %s",
                     (unsigned)type, (unsigned)type, (unsigned)fr,
                     (handler && *handler) ? (*handler)->getMetaClass()->getClassName() : "(none)"); }
            return fr;
        }
#ifdef NMX_ACCEL
        // NVIDIA RM clients are NVK's; RADV and ANV talk to NMXGPU's DRM device nodes instead
        ALOG("newUserClient(0x%x): an NVIDIA RM client type on an AMD/Intel accelerator -- refused", type);
        return kIOReturnUnsupported;
#endif

        if (!locateNVRM()) { ALOG("newUserClient(0x%x): no NVRM service", type); return kIOReturnNotAttached; }

        const OSSymbol *fnSym = OSSymbol::withCStringNoCopy("nvNewAccelClient");
        if (!fnSym) return kIOReturnNoMemory;
        IOUserClient *uc = nullptr;
        IOReturn r = fNVRM->callPlatformFunction(fnSym, false, (void *)owningTask,
                                                 (void *)(uintptr_t)(type & 0xFFFFu), &uc, nullptr);
        fnSym->release();
        if (r != kIOReturnSuccess || !uc) { ALOG("newUserClient(0x%x): NVRM refused (0x%x)", type, r); return r ? r : kIOReturnNoResources; }

        IOService *carrier = nullptr;
        for (unsigned i = 0; i < NV_CARRIERS && !carrier; i++)
            carrier = fCarriers[(fCarrierNext + i) % NV_CARRIERS];
        fCarrierNext++;
        if (!carrier) {
            ALOG("newUserClient(0x%x): NO CARRIER available -- refusing rather than attaching to the "
                 "accelerator, which sleeps at ~1019 children", type);
            uc->release(); return kIOReturnNoResources;
        }
        if (!uc->attach(carrier))  { ALOG("newUserClient(0x%x): attach failed", type); uc->release(); return kIOReturnInternalError; }
        if (!uc->start(carrier))   { ALOG("newUserClient(0x%x): start failed", type); uc->detach(carrier); uc->release(); return kIOReturnInternalError; }
        ALOGV("newUserClient(0x%x) -> %s on the ACCELERATOR (rm type %u)", type, uc->getMetaClass()->getClassName(), type & 0xFFFFu);
        *handler = uc;
        return kIOReturnSuccess;
    }
    static void triggerFromSysctl(int how) {
        NVAccel *a = forDisplay(); NVRetain retained(a);
        if (!a) return;
        if (how >= 4) gNvAperture = 1;
        if (how >= 3) {
            gNvScanout = 1;
            ALOG("  level %d: registering any published head that has no pipe yet", how);
            if (a->fRegLock) IOLockLock(a->fRegLock);
            if (a->registerFramebufferLocked(1))
                ALOG("sysctl debug.nvaccelfb=%d: scanout-backed display pipe ENABLED; the registration just delivered the mode "
                     "change -- not delivering it twice", how);
            else {
                ALOG("sysctl debug.nvaccelfb=%d: scanout-backed display pipe ENABLED, re-delivering the mode change", how);
                a->deliverModeChange();
            }
            if (a->fRegLock) IOLockUnlock(a->fRegLock);
            return;
        }
        a->registerFramebuffer(how);
    }
    static void enableFromHook(const char *why) {
        NVAccel *a = forDisplay(); NVRetain retained(a);
        if (!a) return;
        a->enableAccelerator();
        a->fEnableCount++;
        a->setProperty("NVAccelEnabledFrom", why);
        a->setProperty("NVAccelEnableCount", a->fEnableCount, 32);
    }
    unsigned fEnableCount = 0;
    unsigned fUcLogged = 0;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        ALOG("probe(%s) score %d", provider ? provider->getName() : "?", score ? (int)*score : 0);
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            const unsigned cls = pci->configRead8(0x0B), sub = pci->configRead8(0x0A);
            const unsigned ven = pci->configRead16(0x00), dev = pci->configRead16(0x02);
#ifdef NMX_ACCEL
            static const char *const kind = "AMD/Intel";
            if (cls != 0x03 || (ven != 0x1002 && ven != 0x8086)) {
#else
            static const char *const kind = "NVIDIA";
            if (cls != 0x03 || (sub != 0x00 && sub != 0x02) || ven != 0x10de || dev < 0x0020) {
#endif
                ALOG("%04x:%04x class %02x.%02x is not an %s display device we support - not claiming",
                     ven, dev, cls, sub, kind);
                return NULL;
            }
            ALOG("%s display device %04x:%04x class %02x.%02x - claiming", kind, ven, dev, cls, sub);
        }
        return IOGraphicsAccelerator2::probe(provider, score);
    }
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        { if (nvrm_driver_off()) return false; }
        // Without nvmultigpu=1 the gate is absent. With it, one accelerator starts at a time and an unmarked card waits for the
        // marked one, so cards never run overlapping start-ups (the four-card report).
        NVAccelStartScope scope = { nvaccelMultiGpu(), false };
        {
            uint32_t selector = 0; PE_parse_boot_argn("nvselector", &selector, sizeof selector);
            const bool marked = provider && provider->getProperty("nullmoth-auto-primary") == kOSBooleanTrue;
            if (!nvrm_gate_enter(&gAccelGate, scope.multi, marked, selector != 0)) {
                ALOG("nvmultigpu: another card is starting, or the primary card is not running yet; this one waits to be offered again");
                scope.multi = false;   // nothing was entered, so nothing to leave
                if (provider && gAccelWait.add(provider)) provider->retain();
                if (!__atomic_load_n(&gAccelGate.starting, __ATOMIC_ACQUIRE)) reofferNext();
                return false;
            }
        }
#ifdef NMX_ACCEL
        // NMXGPU decides which AMD/Intel GPUs are driven (nmxamd=1 / nmxintel=1 / nullmoth-gpu); this accelerator only
        // starts on a device it has claimed, so a GPU macOS drives itself never gets a second accelerator
        // No waiting here: NMXGPU attaches from a thread once the disks with its firmware are mounted, and mounting waits
        // for matching to settle - a start() that blocked matching for the claim would hold the boot at the logo. NMXGPU
        // re-runs matching on the device (registerService) after it sets nmxgpu-claimed, which offers it here again.
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            if (pci->getProperty("nmxgpu-claimed") != kOSBooleanTrue) {
                ALOG("NMXGPU has not claimed %04x:%04x yet - not starting now; offered again when it does",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NMXGPU drives %04x:%04x - starting", pci->configRead16(0x00), pci->configRead16(0x02));
        }
#else
        uint32_t gate = 0;
        if (!PE_parse_boot_argn("nvaccel", &gate, sizeof gate) || !gate) { ALOG("boot-arg nvaccel=1 absent: NOT starting (boot-loop guard)"); return false; }
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            unsigned waited = 0;
            while (pci->getProperty("nvrm-claimed") != kOSBooleanTrue && waited < 300) { IOSleep(100); waited++; }
            if (pci->getProperty("nvrm-claimed") != kOSBooleanTrue) {
                ALOG("NVRM never claimed %04x:%04x in 30 s - NOT starting the accelerator on a GPU with no RM",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NVRM owns %04x:%04x (waited %u ms) - starting", pci->configRead16(0x00), pci->configRead16(0x02), waited * 100);
        }
#endif
        fPCI = OSDynamicCast(IOPCIDevice, provider);
        int hybrid = 0;
        const int hasHybrid = PE_parse_boot_argn("nmhybrid", &hybrid, sizeof hybrid);
        gNmHybrid = nmh_level_parse(hasHybrid, hybrid);
        fCandidate = gNmHybrid == NMH_COPY;
        fRegLock = IOLockAlloc();
        if (!gAccelLock) {
            IOLock *lock = IOLockAlloc();
            if (lock && !OSCompareAndSwapPtr(nullptr, lock, (void *volatile *)&gAccelLock)) IOLockFree(lock);
        }
        if (!fPCI || !fRegLock || !gAccelLock) return false;
        IOService *nvrm = locateNVRM();
        fOwner = nvrm ? NVVramOwner::create(nvrm, fPCI, fCandidate) : nullptr;
        if (!fOwner) return false;
        // Capability policy and the retained dispatch target precede the family's service publication.
        if (fCandidate) {
            removeProperty("NVAccelSurfaceWait");
            gNvScanout = 1; gNvAperture = 1; gNvIgpu = 1; gNvIop = 1;
            OSDictionary *caps = OSDictionary::withCapacity(2);
            if (!caps) return false;
            caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
            caps->setObject("TransactionsSupported", kOSBooleanTrue);
            setProperty("IOAccelDisplayPipeCapabilities", caps); caps->release();
        }
        else setProperty("NVAccelSurfaceWait", 1ULL, 32);
        if (registerTarget(true) != kIOReturnSuccess) { fOwner->beginDrain(); return false; }
        if (fCandidate) {
            const char *classes[] = { "NVRMNVDAFramebuffer", "NMIntelFB" };
            for (unsigned k = 0; k < 2; ++k) {
                OSDictionary *matching = serviceMatching(classes[k]);
                OSIterator *it = matching ? getMatchingServices(matching) : nullptr;
                if (matching) matching->release();
                if (it) {
                    while (OSObject *object = it->getNextObject()) {
                        IOService *fb = OSDynamicCast(IOService, object);
                        if (fb) fbPublished(this, nullptr, fb, nullptr);
                    }
                    it->release();
                }
            }
        }
        ALOG("start(%s): calling IOGraphicsAccelerator2::start — the family's own bring-up", provider ? provider->getName() : "?");
        bool r = IOGraphicsAccelerator2::start(provider);
        ALOG("IOGraphicsAccelerator2::start -> %d", r);
        if (r) {
            if (fCandidate) removeProperty("NVAccelSurfaceWait");
            // Properties an Apple accelerator publishes that programs read when they walk the registry for the GPU in use
            // (game engines list IOAccelerator services to pick the primary GPU, and the OpenGL renderer id ties a
            // display GPU to a CGL renderer). Only added when nothing else has set them.
            if (!getProperty("IOAccelRevision")) setProperty("IOAccelRevision", 2ULL, 32);
            if (!getProperty("IOVARendererID")) setProperty("IOVARendererID", 0x01020000ULL | (gAccelTab.count() & 0xffff), 32);
            registerService(); ALOG("registerService() called — Metal can now find MetalPluginName=%s", "NVMTLDriver");
            IOLockLock(gAccelLock);
            gAccelTab.add(this, scope.multi, provider->getProperty("nullmoth-auto-primary") == kOSBooleanTrue);
            IOLockUnlock(gAccelLock);
            gEnableAccel = &NVAccel::enableFromHook;
            publishPrimary();
            scope.started = true;
            if (fCandidate && fDM) {
                IOLockLock(fRegLock);
                const unsigned count = fDM->getFramebufferCount();
                for (unsigned i = 0; i < count && i < 16; ++i) {
                    void *pipe = *(void **)((unsigned char *)fDM + 0x88 + i * 8);
                    unsigned head = 0;
                    if (pipe && pipeFramebuffer(pipe, &head)) {
                        fFBRegistered |= 1u << head; gNvHeadsPiped |= 1u << head;
                    }
                }
                gNvHeadsRegistered = fFBRegistered;
                syncHead4AssociationLocked();
                if (count) onCountGrewLocked();
                IOLockUnlock(fRegLock);
            }
            for (unsigned i = 0; i < NV_CARRIERS; i++) {
                IOAccelNVRMCarrier *c = OSTypeAlloc(IOAccelNVRMCarrier);
                if (!c) break;
                if (!c->init()) { c->release(); break; }
                if (!c->attach(this)) { c->release(); break; }
                if (!c->start(this)) { c->detach(this); c->release(); break; }
                fCarriers[i] = c;
            }
            { unsigned n = 0; for (unsigned i = 0; i < NV_CARRIERS; i++) if (fCarriers[i]) n++;
              ALOG("user-client carriers: %u of %u ready (each holds ~1019 clients before "
                   "IOService::attach starts sleeping)", n, (unsigned)NV_CARRIERS); }
            enableFromHook("start");
            fPCI = OSDynamicCast(IOPCIDevice, provider);
            startTemperature();
            startUtilization();
            ALOG("display machine framebuffer count after the family sweep = %u", fDM ? fDM->getFramebufferCount() : 0xFFFFFFFFu);
            ALOG("multi-head: up to %u heads, registration lock %p%s", kNvMaxFB, fRegLock,
                 fRegLock ? "" : " -- NULL: two sysctl writers could interleave a registration");
#ifdef NMX_ACCEL
            // AMD/Intel: the Metal device is for apps; WindowServer draws into NMXGPU's framebuffer, and the plugin copies
            // finished frames into the IOSurfaces it reads (the headless path NVIDIA laptops use)
            nvaccelInstallSysctl();
            gNvHeadless = 1;
            setProperty("NMXAccelHeadless", kOSBooleanTrue);
            ALOG("AMD/Intel accelerator up: Metal device for apps, frames copied to the framebuffer WindowServer draws");
            OSDictionary *fbm = nullptr;
#else
            OSDictionary *fbm = serviceMatching("NVRMNVDAFramebuffer");
#endif
            if (fbm) {
                fFBNote = addMatchingNotification(gIOFirstPublishNotification, fbm, &NVAccel::fbPublished, this, nullptr, 0);
                fbm->release();
                ALOG("watching for NVRMFramebuffer to publish -> notifier %p", fFBNote);
                // a laptop whose built-in panel is on the Intel GPU: NMIntelFB publishes its own Intel controller local index 0 and maps to logical head 4 of this accelerator
                if (OSDictionary *im = serviceMatching("NMIntelFB")) {
                    fIntelNote = addMatchingNotification(gIOFirstPublishNotification, im, &NVAccel::fbPublished, this, nullptr, 0);
                    im->release();
                    ALOG("watching for an Intel built-in panel (NMIntelFB) -> notifier %p", fIntelNote);
                }
                if (gNmHybrid) if (OSDictionary *gm = serviceMatching("NMGopFB")) {
                    fGopNote = addMatchingNotification(gIOFirstPublishNotification, gm, &NVAccel::fbPublished, this, nullptr, 0);
                    gm->release();
                    ALOG("watching for a firmware-framebuffer panel (NMGopFB) -> notifier %p", fGopNote);
                }
                nvaccelInstallSysctl();
                if (getProperty("MetalPluginName") && (!fCandidate || !hasActiveHead())) {
                    OSObject *mp = getProperty("MetalPluginName"); if (mp) mp->retain();
                    fMetalPlugin = mp;
                    removeProperty("MetalPluginName");
                    ALOG("MetalPluginName withheld -- nothing will load the Metal plugin until "
                         "`sysctl -w debug.nvaccelfb=1` puts it back, and a reboot takes it away again");
                    armHeadlessCheck();
                }
            } else ALOG("serviceMatching(NVRMFramebuffer) returned NULL -- cannot watch for the framebuffer");
        }
        if (!r) { if (fTargetRegistered) registerTarget(false); fOwner->beginDrain(); }
        return r;
    }
    // ---- GPU core temperature ------------------------------------------------
    // Published into our own PerformanceStatistics as "Temperature(C)", the key iStat Menus
    // shows on the GPU panel. NVAccel already owns the IOPCIDevice, so no second kext and no
    // helper process are involved: that is the whole point of doing it here.
    void startTemperature() {
        if (fTempTimer || !fPCI) return;
        IOWorkLoop *wl = getWorkLoop();
        if (!wl) { ALOG("temperature: no work loop -- no temperature will be published"); return; }
        fTempTimer = IOTimerEventSource::timerEventSource(this, &NVAccel::tempTimerFired);
        if (!fTempTimer) { ALOG("temperature: timerEventSource FAILED"); return; }
        if (wl->addEventSource(fTempTimer) != kIOReturnSuccess) {
            ALOG("temperature: addEventSource FAILED");
            fTempTimer->release(); fTempTimer = nullptr; return;
        }
        fTempTimer->setTimeoutMS(kNvTempFirstMs);
        ALOG("temperature: will read TSENS 0x%05x every %u ms", kNvTsensOffset, kNvTempIntervalMs);
    }

    void stopTemperature() {
        if (fTempTimer) {
            fTempTimer->cancelTimeout();
            if (IOWorkLoop *wl = getWorkLoop()) wl->removeEventSource(fTempTimer);
            fTempTimer->release(); fTempTimer = nullptr;
        }
        if (fTempMap) { fTempMap->release(); fTempMap = nullptr; }
        fTempRegs = nullptr;
    }

    static void tempTimerFired(OSObject *owner, IOTimerEventSource *sender) {
        (void)sender;
        // Registered with `this` as the owner, so the cast cannot fail; OSDynamicCast is not
        // used because it needs the complete class, and this body sits inside the class.
        NVAccel *me = (NVAccel *)owner;
        if (!me) return;
        me->sampleTemperature();
        me->publishStatistics();
        if (me->fTempTimer) me->fTempTimer->setTimeoutMS(kNvTempIntervalMs);
    }

    void sampleTemperature() {
        if (!fTempRegs) {
            fTempMap = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, 0);
            if (!fTempMap) { ALOG("temperature: BAR0 map FAILED -- giving up"); return; }
            fTempRegs = (volatile UInt32 *)fTempMap->getVirtualAddress();
            if (!fTempRegs) { ALOG("temperature: BAR0 has no virtual address -- giving up"); return; }
        }
        UInt32 raw = fTempRegs[kNvTsensOffset / 4];
        unsigned val = (raw & kNvTsensValueMask) >> 8;
        // A GPU in a low power state can answer 0xffffffff, and 0 means the sensor has not been
        // clocked yet: both fall outside the range and are left unpublished rather than guessed.
        if (raw == 0xffffffffu || val < kNvTempMinC || val > kNvTempMaxC) {
            if (fTempFails++ == 0)
                ALOG("temperature: TSENS raw=0x%08x outside %u..%u C -- not publishing",
                     raw, kNvTempMinC, kNvTempMaxC);
            return;
        }
        fTempFails = 0;
        if (!fTempLogged) { fTempLogged = true; ALOG("temperature: first reading raw=0x%08x -> %u C", raw, val); }
        fTempC = (int)val;
    }

    void startUtilization() {
#ifndef NMX_ACCEL
        if (!fPCI || fUtilLoop) return;
        fUtilLock = IOLockAlloc();
        fUtilLoop = IOWorkLoop::workLoop();
#if defined(NM_ERA) && NM_ERA < 1013
        // The options form arrived in 10.13; the plain timer runs on the same private work loop.
        fUtilTimer = IOTimerEventSource::timerEventSource(this, &NVAccel::utilTimerFired);
#else
        fUtilTimer = IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityWorkLoop,
                                                        this, &NVAccel::utilTimerFired);
#endif
        if (!fUtilLock || !fUtilLoop || !fUtilTimer ||
            fUtilLoop->addEventSource(fUtilTimer) != kIOReturnSuccess) {
            ALOG("utilization: cannot create sampling work loop");
            stopUtilization();
            return;
        }
        fUtilTimer->setTimeoutMS(kNvTempFirstMs);
#endif
    }

    void stopUtilization() {
        if (fUtilTimer) {
            fUtilTimer->cancelTimeout();
            // Removing the source takes the loop gate, so an in-flight RM query finishes
            // before its provider reference and the cache lock can be released.
            if (fUtilLoop && fUtilTimer->getWorkLoop()) fUtilLoop->removeEventSource(fUtilTimer);
            // The callback may have rearmed itself while removal waited for the gate.
            fUtilTimer->cancelTimeout();
            fUtilTimer->release(); fUtilTimer = nullptr;
        }
        if (fUtilLoop) { fUtilLoop->release(); fUtilLoop = nullptr; }
        if (fUtilProvider) { fUtilProvider->release(); fUtilProvider = nullptr; }
        if (fUtilLock) { IOLockFree(fUtilLock); fUtilLock = nullptr; }
        fUtilValid = false;
    }

    static void utilTimerFired(OSObject *owner, IOTimerEventSource *sender) {
        NVAccel *me = (NVAccel *)owner;
        me->sampleUtilization();
        sender->setTimeoutMS(kNvUtilIntervalMs);
    }

    void sampleUtilization() {
        if (fUtilProvider && fUtilProvider->isInactive()) {
            fUtilProvider->release(); fUtilProvider = nullptr;
        }
        if (!fUtilProvider) {
            // Match the RM service on this PCI device, rather than a global GPU 0.
            OSIterator *clients = fPCI->getClientIterator();
            if (clients) {
                while (OSObject *o = clients->getNextObject()) {
                    IOService *service = OSDynamicCast(IOService, o);
                    if (service && service->metaCast("NVRM") && !service->isInactive()) {
                        service->retain(); fUtilProvider = service; break;
                    }
                }
                clients->release();
            }
        }
        NVRMUtilization sample = {};
        sample.version = NVRM_UTILIZATION_ABI_VERSION;
        IOReturn status = kIOReturnNotAttached;
        if (fUtilProvider && !fUtilProvider->isInactive()) {
            const OSSymbol *fn = OSSymbol::withCStringNoCopy(NVRM_UTILIZATION_FN);
            if (fn) {
                status = fUtilProvider->callPlatformFunction(fn, false, &sample, nullptr, nullptr, nullptr);
                fn->release();
            }
        }
        IOLockLock(fUtilLock);
        if (status == kIOReturnSuccess && sample.version == NVRM_UTILIZATION_ABI_VERSION &&
            sample.timestamp && sample.gr <= 10000 && sample.fb <= 10000 &&
            sample.nvenc <= 10000 && sample.nvdec <= 10000) {
            // A successful query may return the same firmware sample. That must not
            // indefinitely refresh a cached value after the GPU stops sampling.
            if (!fUtilUpdated || sample.timestamp != fUtil.timestamp) {
                fUtil = sample;
                fUtilUpdated = mach_continuous_time();
            }
            fUtilValid = true;
        } else {
            fUtilValid = false;
        }
        IOLockUnlock(fUtilLock);
    }

    // Read-copy-modify-write: the family keeps 70-odd keys in this dictionary and replaces it
    // wholesale every 0.25-2 s, so the copy is taken from whatever is in it at this moment.
    void publishStatistics() {
        OSObject *property = copyProperty("PerformanceStatistics");
        OSDictionary *cur = OSDynamicCast(OSDictionary, property);
        OSDictionary *d = cur ? OSDynamicCast(OSDictionary, cur->copyCollection()) : nullptr;
        if (property) property->release();
        if (!d) { d = OSDictionary::withCapacity(8); if (!d) return; }
        if (fTempC) {
            OSNumber *n = OSNumber::withNumber((unsigned long long)fTempC, 32);
            if (n) { d->setObject("Temperature(C)", n); n->release(); }
        }
#ifndef NMX_ACCEL
        NVRMUtilization sample = {};
        bool valid = false;
        if (fUtilLock) {
            IOLockLock(fUtilLock);
            uint64_t age = 0;
            if (fUtilValid) absolutetime_to_nanoseconds(mach_continuous_time() - fUtilUpdated, &age);
            valid = fUtilValid && age <= kNvUtilMaxAgeNs;
            sample = fUtil;
            IOLockUnlock(fUtilLock);
        }
        const char *keys[] = { "Device Utilization %", "GPU Core Utilization %",
                               "GPU Memory Utilization %", "GPU Video Engine Utilization %",
                               "GPU Video Decode Utilization %" };
        const unsigned values[] = { sample.gr, sample.gr, sample.fb, sample.nvenc, sample.nvdec };
        for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
            if (valid) {
                OSNumber *n = OSNumber::withNumber((unsigned long long)((values[i] + 50) / 100), 32);
                if (n) { d->setObject(keys[i], n); n->release(); }
            } else {
                // Missing telemetry is not an idle GPU; remove stale values instead of
                // turning errors, sleep or an unsupported firmware command into 0%.
                d->removeObject(keys[i]);
            }
        }
#endif
        setProperty("PerformanceStatistics", d);
        d->release();
    }

    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE {
        __atomic_store_n(&fStopping, true, __ATOMIC_RELEASE);
        if (fOwner) fOwner->beginDrain();
        if (fTargetRegistered) registerTarget(false);
        if (gAccelLock) {
            IOLockLock(gAccelLock); gAccelTab.remove(this, nvaccelMultiGpu()); IOLockUnlock(gAccelLock);
        }
        if (fFBNote) { fFBNote->remove(); fFBNote = nullptr; }
        if (fIntelNote) { fIntelNote->remove(); fIntelNote = nullptr; }
        if (fGopNote) { fGopNote->remove(); fGopNote = nullptr; }
        if (fHeadlessCall) {
            if (fRegLock) IOLockLock(fRegLock);
            if (thread_call_cancel(fHeadlessCall)) __atomic_store_n(&fHeadlessArmed, 0, __ATOMIC_RELEASE);
            if (fRegLock) IOLockUnlock(fRegLock);
            while (__atomic_load_n(&fHeadlessArmed, __ATOMIC_ACQUIRE)) {
                if (fRegLock) IOLockLock(fRegLock);
                if (thread_call_cancel(fHeadlessCall)) __atomic_store_n(&fHeadlessArmed, 0, __ATOMIC_RELEASE);
                if (fRegLock) IOLockUnlock(fRegLock);
                IOSleep(1);
            }
            while (!thread_call_free(fHeadlessCall)) { thread_call_cancel(fHeadlessCall); IOSleep(1); }
            fHeadlessCall = nullptr;
        }
        stopTemperature();
        stopUtilization();
        if (fOwner) fOwner->waitOperations();
        nvAccelDrainPresent(this);
        if (fRegLock) { IOLockLock(fRegLock); clearHead4AssociationLocked(); IOLockUnlock(fRegLock); }
        // Native teardown may still unmap and complete memory; the child owner keeps the device alive.
        IOGraphicsAccelerator2::stop(provider);
        fDM = nullptr;
        if (fStamps) { fStamps->complete(); fStamps->release(); fStamps = nullptr; }
        for (unsigned i = 0; i < 32; ++i) if (fRanges[i]) { fRanges[i]->release(); fRanges[i] = nullptr; }
        for (unsigned i = 0; i < NV_CARRIERS; ++i) if (fCarriers[i]) {
            fCarriers[i]->stop(this); fCarriers[i]->detach(this); fCarriers[i]->release(); fCarriers[i] = nullptr;
        }
        if (fFB) { fFB->release(); fFB = nullptr; }
        for (unsigned i = 0; i < kNvMaxFB; ++i) if (fFBs[i]) { fFBs[i]->release(); fFBs[i] = nullptr; }
        if (fNVRM) { fNVRM->release(); fNVRM = nullptr; }
        if (fMetalPlugin) { fMetalPlugin->release(); fMetalPlugin = nullptr; }
        if (fSymScanout) { fSymScanout->release(); fSymScanout = nullptr; }
        if (fOwner) { fOwner->scheduleReap(); fOwner->release(); fOwner = nullptr; }
        publishPrimary();
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fRegLock) { IOLockFree(fRegLock); fRegLock = nullptr; }
        if (fOwner) { fOwner->beginDrain(); fOwner->release(); fOwner = nullptr; }
        if (fNVRM) { fNVRM->release(); fNVRM = nullptr; }
        IOGraphicsAccelerator2::free();
    }
    virtual IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVAccel, IOGraphicsAccelerator2)
NVRetain::NVRetain(NVAccel *a) : object(a), display(a != nullptr) {}
NVRetain::~NVRetain() { if (display) ((NVAccel *)object)->fOwner->leave(); if (object) object->release(); }
IOLock *NVAccel::gAccelLock = nullptr;
NvrmDevTab<NVAccel> NVAccel::gAccelTab;
NvrmStartGate NVAccel::gAccelGate;
NvrmWaitQueue<IOService> NVAccel::gAccelWait;
NVAccelStartScope::~NVAccelStartScope() { nvrm_gate_leave(&NVAccel::gAccelGate, multi, started); if (multi) NVAccel::reofferNext(); }

OSDefineMetaClassAndStructors(NVVramOwner, IOService)
OSDefineMetaClassAndStructors(NVStockLifetime, IOService)
OSDefineMetaClassAndStructors(NVVramAllocation, OSObject)
OSDefineMetaClassAndStructors(NVVramMemoryDescriptor, IOGeneralMemoryDescriptor)
OSDefineMetaClassAndStructors(NVGpuVaRecord, OSObject)
static uint64_t gOwnerGeneration;

NVVramOwner *NVVramOwner::create(IOService *provider, IOPCIDevice *pci, bool direct)
{
    if (!provider || !pci || provider->getProvider() != pci || provider->isInactive()) return nullptr;
    NVVramOwner *o = OSTypeAlloc(NVVramOwner);
    if (!o || !o->init()) { if (o) o->release(); return nullptr; }
    o->fLock = IOLockAlloc();
    o->fReaper = thread_call_allocate_with_options(&NVVramOwner::reapCall, o,
        THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (!o->fLock || !o->fReaper) { o->release(); return nullptr; }
    uint64_t previous = __atomic_load_n(&gOwnerGeneration, __ATOMIC_ACQUIRE), generation;
    do {
        generation = previous;
        if (!nvNextGeneration(&generation, &generation)) { o->release(); return nullptr; }
    } while (!__atomic_compare_exchange_n(&gOwnerGeneration, &previous, generation, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    o->fState.activate(generation);
    o->fNvrm = provider; provider->retain();
    o->fPci = pci; pci->retain(); o->fDirect = direct;
    IODeviceMemory *bar = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
    o->fBarBytes = bar ? bar->getLength() : 0;
    if (!o->setProperty(NVRM_OWNER_GENERATION_PROPERTY, generation, 64) || !o->attach(provider)) { o->release(); return nullptr; }
    o->fAttached = true;
    if (!o->start(provider)) { o->detach(provider); o->fAttached = false; o->release(); return nullptr; }
    o->retain(); o->fLifetimeHeld = true;
    o->registerService();
    return o;
}
IOReturn NVVramOwner::call(const char *name, void *request)
{
    if (!fNvrm) return kIOReturnNotAttached;
    const OSSymbol *symbol = OSSymbol::withCStringNoCopy(name);
    if (!symbol) return kIOReturnNoMemory;
    IOReturn r = fNvrm->callPlatformFunction(symbol, false, request, nullptr, nullptr, nullptr);
    symbol->release(); return r;
}
bool NVVramOwner::bindDisplay(IOService *fb, unsigned head)
{
    if (!fb || head >= 5 || !enter()) return false;
    IORegistryEntry *parent = fb;
    for (unsigned depth = 0; parent && parent != fPci && depth < 8; ++depth) parent = parent->getParentEntry(gIOServicePlane);
    if (parent != fPci) { leave(); return false; }
    IOLockLock(fLock);
    if (fDisplayLifetime[head] || fDisplayBinding[head] || fState.phase != NVOwnerPhase::Active) {
        const bool same = fState.phase == NVOwnerPhase::Active && fDisplayLifetime[head] &&
                          fDisplayLifetime[head]->fFramebuffer == fb;
        IOLockUnlock(fLock); leave(); return same;
    }
    fDisplayBinding[head] = true;
    IOLockUnlock(fLock);
    NVStockLifetime *guard = OSTypeAlloc(NVStockLifetime);
    const bool initialized = guard && guard->init();
    if (initialized) { guard->fOwner = this; guard->fFramebuffer = fb; fb->retain(); }
    const bool attached = initialized && guard->attach(fb);
    const bool started = attached && guard->start(fb);
    IOLockLock(fLock);
    const bool published = started && !fb->isInactive() && fState.phase == NVOwnerPhase::Active;
    if (published) fDisplayLifetime[head] = guard;
    fDisplayBinding[head] = false;
    IOLockUnlock(fLock);
    if (!published && guard) {
        guard->fOwner = nullptr;
        if (guard->fDeferred) { bool defer = false; guard->IOService::didTerminate(fb, guard->fOptions, &defer); }
        else { if (started) guard->stop(fb); if (attached) guard->detach(fb); }
        guard->release();
    }
    leave();
    return published;
}
bool NVVramOwner::ensureDeviceLocked()
{
    if (fDevice) return fState.phase == NVOwnerPhase::Active;
    if (fInitializing || !fState.enter()) return false;
    fInitializing = true;
    IOLockUnlock(fLock);
    IOService *stock = nullptr;
    NVStockLifetime *guard = nullptr;
    bool attached = false, started = false;
    NvKmsKapiFunctionsTable *kms = nullptr;
    NvKmsKapiDevice *device = nullptr;
    NVRMRmHandles handles = {}; handles.version = NVRM_GPUVA_ABI_VERSION;
    if (!fNvrm->isInactive() && !fDirect) {
        OSDictionary *matching = IOService::serviceMatching("NVRMNVDAFramebuffer");
        OSIterator *it = matching ? IOService::getMatchingServices(matching) : nullptr;
        if (matching) matching->release();
        if (it) {
            while (OSObject *object = it->getNextObject()) {
                IOService *fb = OSDynamicCast(IOService, object);
                IORegistryEntry *p = fb;
                for (unsigned depth = 0; p && p != fPci && depth != 8; ++depth) p = p->getParentEntry(gIOServicePlane);
                if (fb && p == fPci && !fb->isInactive()) { stock = fb; stock->retain(); break; }
            }
            it->release();
        }
        if (stock) {
            guard = OSTypeAlloc(NVStockLifetime);
            if (guard && guard->init()) {
                guard->fOwner = this; guard->fFramebuffer = stock; stock->retain();
                attached = guard->attach(stock);
                started = attached && guard->start(stock);
                if (started) {
                    const OSSymbol *symbol = OSSymbol::withCStringNoCopy(NVRM_GPUVA_FN_HANDLES);
                    const IOReturn result = symbol ? stock->callPlatformFunction(symbol, false, &handles, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
                    if (symbol) symbol->release();
                    if (result == kIOReturnSuccess) device = (NvKmsKapiDevice *)handles.kapiDevice;
                }
            }
        }
    } else if (!fNvrm->isInactive()) {
        NVRMDisplayPointers dp = {}; dp.version = NVRM_DISPLAY_PTRS_ABI_VERSION;
        if (call(NVRM_DISPLAY_PTRS_FN, &dp) == kIOReturnSuccess) kms = (NvKmsKapiFunctionsTable *)dp.kapi;
        if (kms && kms->allocateDevice && kms->freeDevice && kms->allocateMemory &&
            kms->freeMemory && kms->mapMemory && kms->unmapMemory) {
            NvU32 gpuId = 0; bool found = false;
            OSIterator *clients = fNvrm->getClientIterator();
            if (clients) {
                while (OSObject *object = clients->getNextObject()) {
                    IOService *s = OSDynamicCast(IOService, object);
                    OSNumber *n = s ? OSDynamicCast(OSNumber, s->getProperty("gpu-id")) : nullptr;
                    if (n) { gpuId = n->unsigned32BitValue(); found = true; break; }
                }
                clients->release();
            }
            if (found) {
                NvKmsKapiAllocateDeviceParams ap = {}; ap.gpuId = gpuId;
                device = kms->allocateDevice(&ap);
                handles.kapiDevice = device;
            }
        }
    }
    IOLockLock(fLock);
    const bool published = device && fState.phase == NVOwnerPhase::Active;
    if (published) {
        fDevice = device; fKms = kms; fHandles = handles;
        fStock = stock; fStockLifetime = guard;
    }
    IOLockUnlock(fLock);
    if (!published) {
        if (fDirect && device) kms->freeDevice(device);
        if (guard) {
            guard->fOwner = nullptr;
            if (guard->fDeferred) { bool defer = false; guard->IOService::didTerminate(stock, guard->fOptions, &defer); }
            else { if (started) guard->stop(stock); if (attached) guard->detach(stock); }
            guard->release();
        }
        if (stock) stock->release();
    }
    IOLockLock(fLock);
    fInitializing = false;
    fState.leave();
    IOLockWakeup(fLock, &fState.operations, false);
    return published && fState.phase == NVOwnerPhase::Active;
}
bool NVVramOwner::active()
{ IOLockLock(fLock); bool ok = fState.phase == NVOwnerPhase::Active; IOLockUnlock(fLock); return ok; }
bool NVVramOwner::enter()
{ IOLockLock(fLock); bool ok = fState.enter(); IOLockUnlock(fLock); return ok; }
void NVVramOwner::leave()
{ IOLockLock(fLock); fState.leave(); IOLockWakeup(fLock, &fState.operations, false); IOLockUnlock(fLock); scheduleReap(); }
void NVVramOwner::waitOperations()
{ IOLockLock(fLock); while (fState.operations) IOLockSleep(fLock, &fState.operations, THREAD_UNINT); IOLockUnlock(fLock); }
bool NVVramOwner::borrow()
{
    IOLockLock(fLock);
    bool ok = fState.phase == NVOwnerPhase::Active && fState.borrowers != UINT32_MAX;
    if (ok) { ++fState.borrowers; retain(); }
    IOLockUnlock(fLock); return ok;
}
void NVVramOwner::unborrow()
{
    IOLockLock(fLock); if (fState.borrowers) --fState.borrowers; IOLockUnlock(fLock);
    scheduleReap(); release();
}
bool NVVramOwner::handles(NVRMRmHandles *out)
{
    if (!out || !enter()) return false;
    IOLockLock(fLock);
    bool ok = fState.phase == NVOwnerPhase::Active && ensureDeviceLocked();
    if (ok) *out = fHandles;
    IOLockUnlock(fLock);
    leave();
    return ok;
}
IOReturn NVVramOwner::cpuAccess(IOReturn (*body)(void *), void *context)
{
    if (!body || !enter()) return kIOReturnNotReady;
    NVRMOwnerCpuAccess q = { NVRM_OWNER_ABI_VERSION, sizeof(q), this, fState.generation, body, context };
    IOReturn r = call(NVRM_OWNER_CPU_ACCESS_FN, &q);
    leave(); return r;
}
NVVramAllocation *NVVramOwner::allocate(uint64_t bytes)
{
    const uint64_t want = nm_optc_align_vram_size(bytes);
    if (!want || !enter()) return nullptr;
    NVVramAllocation *a = OSTypeAlloc(NVVramAllocation);
    if (!a || !a->init()) { if (a) a->release(); leave(); return nullptr; }
    IOLockLock(fLock);
    uint64_t generation = 0;
    const uint64_t budget = fDirect ? nm_optc_vram_budget(fBarBytes) : UINT64_MAX;
    bool ok = fState.phase == NVOwnerPhase::Active && ensureDeviceLocked() &&
        fState.allocations != UINT32_MAX && fMappedBytes <= budget && want <= budget - fMappedBytes &&
        nvNextGeneration(&fState.nextAllocation, &generation);
    if (ok) { ++fState.allocations; fMappedBytes += want; }
    IOLockUnlock(fLock);
    if (!ok) { a->release(); leave(); return nullptr; }

    NVRMVramRequest r = {}; r.version = NVRM_VRAM_ABI_VERSION; r.size = want;
    if (fDirect) {
        NvKmsKapiAllocateMemoryParams mp = {}; NvU8 compressible = 0;
        mp.layout = NvKmsSurfaceMemoryLayoutPitch; mp.type = NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT;
        mp.size = want; mp.useVideoMemory = NV_TRUE; mp.compressible = &compressible;
        r.handle = fKms->allocateMemory(fDevice, &mp);
        ok = r.handle && fKms->mapMemory(fDevice, (NvKmsKapiMemory *)r.handle, NVKMS_KAPI_MAPPING_TYPE_KERNEL, &r.kva) && r.kva;
        r.actualSize = want; r.allocationCookie = generation;
    } else {
        const OSSymbol *s = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_ALLOC);
        ok = s && !fStock->isInactive() && fStock->callPlatformFunction(s, false, &r, nullptr, nullptr, nullptr) == kIOReturnSuccess;
        if (s) s->release();
    }
    NVRMKernelMapInfo mi = { NVRM_OWNER_ABI_VERSION, sizeof(mi), r.kva, want, 0, 0, 0, 0 };
    ok = ok && r.handle && r.kva && r.actualSize >= want &&
         call(NVRM_KERNEL_MAP_INFO_FN, &mi) == kIOReturnSuccess && mi.physical && mi.availableBytes >= r.actualSize &&
         (mi.cacheOptions == kIOMapInhibitCache || mi.cacheOptions == kIOMapWriteCombineCache);
    if (ok) {
        struct Clear { void *kva; size_t size; } clear = { r.kva, (size_t)r.actualSize };
        NVRMOwnerCpuAccess access = { NVRM_OWNER_ABI_VERSION, sizeof(access), this, fState.generation,
            [](void *p) -> IOReturn { Clear *c = (Clear *)p; bzero(c->kva, c->size); nv_frame_store_drain(); return kIOReturnSuccess; }, &clear };
        ok = call(NVRM_OWNER_CPU_ACCESS_FN, &access) == kIOReturnSuccess;
    }
    IOLockLock(fLock);
    uint64_t charged = want;
    if (r.actualSize > want) {
        const uint64_t extra = r.actualSize - want;
        if (fMappedBytes <= budget && extra <= budget - fMappedBytes) {
            fMappedBytes += extra; charged += extra;
        } else {
            ok = false; fState.drain(); fHandles = {};
        }
    }
    ok = ok && fState.phase == NVOwnerPhase::Active;
    if (r.handle) {
        r.phys = mi.physical;
        a->fOwner = this; retain(); a->fState.key = { fState.generation, generation };
        a->fReceipt = r; a->fCache = mi.cacheOptions; a->fChargedBytes = charged;
        if (!ok) { a->fState.retire(); fRetirementPending = true; }
        a->fNext = fAllocations; fAllocations = a; a->retain();
    } else {
        --fState.allocations; fMappedBytes -= charged;
    }
    IOLockUnlock(fLock); leave();
    if (!ok) { a->release(); return nullptr; }
    return a;
}
void NVVramOwner::beginDrain()
{
    if (!fLock) return;
    IOLockLock(fLock);
    fState.drain(); fHandles = {};
    for (NVVramAllocation *a = fAllocations; a; a = a->fNext) a->fState.retire();
    IOLockUnlock(fLock);
    removeProperty(NVRM_OWNER_GENERATION_PROPERTY);
    scheduleReap();
}
bool NVVramOwner::isDrained()
{ IOLockLock(fLock); bool done = fState.phase == NVOwnerPhase::Dead; IOLockUnlock(fLock); return done; }
void NVVramOwner::scheduleReap(unsigned delayMs)
{
    if (!fReaper || !fLock) return;
    uint64_t deadline = 0; clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
    IOLockLock(fLock);
    if (fState.phase != NVOwnerPhase::Dead &&
        (fRetirementPending || fState.phase == NVOwnerPhase::Draining))
        thread_call_enter_delayed(fReaper, deadline);
    IOLockUnlock(fLock);
}
void NVVramOwner::reapCall(void *p, void *) { ((NVVramOwner *)p)->reap(); }
bool NVVramOwner::retireScanout(unsigned head, IOService *fb, OSObject *frontSurface, OSObject *previousSurface,
                                NVVramAllocation *front, NVVramAllocation *previous)
{
    if (head >= 5 || !fb) return false;
    if (!front && !previous && !frontSurface && !previousSurface) return true;
    IOLockLock(fLock);
    if (fRetiredScanout[head].framebuffer || fState.borrowers == UINT32_MAX) { IOLockUnlock(fLock); return false; }
    fRetiredScanout[head] = { fb, frontSurface, previousSurface, front, previous };
    fb->retain(); retain(); ++fState.borrowers; fRetirementPending = true;
    IOLockUnlock(fLock);
    scheduleReap(); return true;
}
bool NVVramOwner::scanoutRetiring(unsigned head)
{
    if (head >= 5) return true;
    IOLockLock(fLock); bool pending = fRetiredScanout[head].framebuffer != nullptr; IOLockUnlock(fLock); return pending;
}
bool NVVramOwner::reapScanouts()
{
    bool pending = false;
    for (unsigned head = 0; head < 5; ++head) {
        IOLockLock(fLock); NVRetiredScanout record = fRetiredScanout[head]; IOLockUnlock(fLock);
        if (!record.framebuffer) continue;
        NVRMFlipRequest q = {}; q.version = NVRM_FLIP_ABI_VERSION; q.flags = 1;
        const OSSymbol *symbol = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
        IOReturn result = symbol ? record.framebuffer->callPlatformFunction(symbol, false, &q, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
        if (symbol) symbol->release();
        if (result != kIOReturnSuccess) { pending = true; continue; }
        IOLockLock(fLock); fRetiredScanout[head] = {}; --fState.borrowers; IOLockUnlock(fLock);
        if (record.frontSurface) record.frontSurface->release();
        if (record.previousSurface) record.previousSurface->release();
        if (record.front) { record.front->releaseLease(NVLeaseUse::Present); record.front->release(); }
        if (record.previous) { record.previous->releaseLease(NVLeaseUse::Present); record.previous->release(); }
        record.framebuffer->release(); release();
    }
    return pending;
}
void NVVramOwner::reap()
{
    IOLockLock(fLock);
    if (fReaping) { IOLockUnlock(fLock); return; }
    fReaping = true;
    IOLockUnlock(fLock);
    bool retry = reapScanouts(), finished = false, deferred = false;
    IOLockLock(fLock);
    NVGpuVaRecord **v = &fReservations;
    while (*v) {
        NVGpuVaRecord *r = *v;
        if (!r->fRetiring) { v = &r->fNext; continue; }
        r->retain(); IOLockUnlock(fLock);
        bool done = r->unmap(true);
        IOLockLock(fLock);
        if (done) {
            v = &fReservations; while (*v && *v != r) v = &(*v)->fNext;
            if (*v) { *v = r->fNext; --fState.reservations; IOLockUnlock(fLock); r->release(); IOLockLock(fLock); }
        } else { retry = true; v = &r->fNext; }
        IOLockUnlock(fLock); r->release(); IOLockLock(fLock);
    }
    NVVramAllocation **link = &fAllocations;
    while (*link) {
        if (fState.operations) break;
        NVVramAllocation *a = *link;
        if (!a->fState.releasable()) { link = &a->fNext; continue; }
        IOLockUnlock(fLock);
        bool ok = true;
        if (fDirect) {
            if (a->fReceipt.kva)
                fKms->unmapMemory(fDevice, (NvKmsKapiMemory *)a->fReceipt.handle, NVKMS_KAPI_MAPPING_TYPE_KERNEL, a->fReceipt.kva);
            fKms->freeMemory(fDevice, (NvKmsKapiMemory *)a->fReceipt.handle);
        } else {
            NVRMVramRequest receipt = a->fReceipt;
            const OSSymbol *s = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_FREE);
            ok = s && fStock->callPlatformFunction(s, false, &receipt, nullptr, nullptr, nullptr) == kIOReturnSuccess;
            if (s) s->release();
        }
        IOLockLock(fLock);
        link = &fAllocations; while (*link && *link != a) link = &(*link)->fNext;
        if (!ok) { retry = true; link = &a->fNext; continue; }
        *link = a->fNext; --fState.allocations; fMappedBytes -= a->fChargedBytes;
        a->fState.phase = NVBackingPhase::Released;
        IOLockUnlock(fLock); a->release(); IOLockLock(fLock);
    }
    fRetirementPending = retry;
    for (NVVramAllocation *a = fAllocations; a; a = a->fNext)
        if (a->fState.phase == NVBackingPhase::Retiring) fRetirementPending = true;
    if (fState.drained()) {
        NVRMGpuVaRequest q = {}; q.version = NVRM_GPUVA_ABI_VERSION; q.kapiDevice = fDevice;
        IOLockUnlock(fLock);
        const bool drained = !fDevice || call(NVRM_GPUVA_DRAIN_FN, &q) == kIOReturnSuccess;
        if (drained && fDirect && fDevice) fKms->freeDevice(fDevice);
        IOLockLock(fLock);
        if (drained) {
            fDevice = nullptr; fKms = nullptr; fState.phase = NVOwnerPhase::Dead;
            finished = true; deferred = fDeferred; fDeferred = false;
        } else retry = true;
    }
    IOLockUnlock(fLock);
    if (!finished) {
        if (retry) scheduleReap(100);
        IOLockLock(fLock); fReaping = false; IOLockUnlock(fLock);
        return;
    }
    if (finished) {
        thread_call_cancel(fReaper); // discard a retirement wake queued before the final Dead transition
        for (unsigned head = 0; head < 5; ++head) if (fDisplayLifetime[head]) {
            NVStockLifetime *guard = fDisplayLifetime[head]; fDisplayLifetime[head] = nullptr;
            guard->fOwner = nullptr;
            if (guard->fDeferred) { bool defer = false; guard->IOService::didTerminate(guard->fFramebuffer, guard->fOptions, &defer); }
            else { guard->stop(guard->fFramebuffer); guard->detach(guard->fFramebuffer); }
            guard->release();
        }
        if (fStockLifetime) {
            NVStockLifetime *guard = fStockLifetime; fStockLifetime = nullptr;
            guard->fOwner = nullptr;
            if (guard->fDeferred) { bool defer = false; guard->IOService::didTerminate(fStock, guard->fOptions, &defer); }
            else { guard->stop(fStock); guard->detach(fStock); }
            guard->release();
        }
        if (deferred) { bool defer = false; IOService::didTerminate(fNvrm, fTerminateOptions, &defer); }
        else if (fAttached) { stop(fNvrm); detach(fNvrm); fAttached = false; }
        if (fLifetimeHeld) { fLifetimeHeld = false; release(); }
    }
}
void NVStockLifetime::free()
{ if (fFramebuffer) fFramebuffer->release(); IOService::free(); }
bool NVStockLifetime::willTerminate(IOService *provider, IOOptionBits options)
{ if (fOwner) fOwner->beginDrain(); return IOService::willTerminate(provider, options); }
bool NVStockLifetime::didTerminate(IOService *provider, IOOptionBits options, bool *defer)
{
    NVVramOwner *owner = fOwner;
    if (owner) {
        IOLockLock(owner->fLock);
        fOptions = options; fDeferred = owner->fState.phase != NVOwnerPhase::Dead; *defer = fDeferred;
        IOLockUnlock(owner->fLock);
    }
    return IOService::didTerminate(provider, options, defer);
}
bool NVVramOwner::willTerminate(IOService *provider, IOOptionBits options)
{ beginDrain(); return IOService::willTerminate(provider, options); }
bool NVVramOwner::didTerminate(IOService *provider, IOOptionBits options, bool *defer)
{
    IOLockLock(fLock);
    fDeferred = fState.phase != NVOwnerPhase::Dead; fTerminateOptions = options; *defer = fDeferred;
    IOLockUnlock(fLock);
    return IOService::didTerminate(provider, options, defer);
}
void NVVramOwner::free()
{
    if (fReaper) { thread_call_free(fReaper); fReaper = nullptr; }
    if (fStock) fStock->release();
    if (fPci) fPci->release();
    if (fNvrm) fNvrm->release();
    if (fLock) IOLockFree(fLock);
    IOService::free();
}
NVVramLease::~NVVramLease() { reset(); }
void NVVramLease::reset()
{ NVVramAllocation *a = fAllocation; fAllocation = nullptr; if (a) { a->releaseLease(fUse); a->release(); } }
bool NVVramAllocation::acquire(NVLeaseUse use, NVVramLease *lease)
{
    if (!lease || lease->get()) return false;
    IOLockLock(fOwner->fLock);
    bool ok = fState.acquire(fOwner->fState.phase, use);
    if (ok) { retain(); lease->fAllocation = this; lease->fUse = use; }
    IOLockUnlock(fOwner->fLock); return ok;
}
void NVVramAllocation::releaseLease(NVLeaseUse use)
{ IOLockLock(fOwner->fLock); fState.release(use); IOLockUnlock(fOwner->fLock); fOwner->scheduleReap(); }
void NVVramAllocation::retire()
{ IOLockLock(fOwner->fLock); fState.retire(); fOwner->fRetirementPending = true; IOLockUnlock(fOwner->fLock); fOwner->scheduleReap(); }
bool NVVramAllocation::park()
{ IOLockLock(fOwner->fLock); bool ok = fOwner->fState.phase == NVOwnerPhase::Active && fState.park(); IOLockUnlock(fOwner->fLock); return ok; }
bool NVVramAllocation::unpark()
{ IOLockLock(fOwner->fLock); bool ok = fOwner->fState.phase == NVOwnerPhase::Active && fState.unpark(); IOLockUnlock(fOwner->fLock); return ok; }
void NVVramAllocation::free() { if (fOwner) fOwner->release(); OSObject::free(); }
bool NVVramMemoryDescriptor::initWithAllocation(NVVramAllocation *a, uint64_t offset, uint64_t length)
{
    if (!a || !length || offset > a->fReceipt.actualSize || length > a->fReceipt.actualSize - offset ||
        a->fReceipt.phys > UINT64_MAX - offset || !a->acquire(NVLeaseUse::CpuAccess, &fLease)) return false;
    IOAddressRange range = { a->fReceipt.phys + offset, length };
    if (!IOGeneralMemoryDescriptor::initWithOptions(&range, 1, 0, nullptr,
            kIOMemoryTypePhysical64 | kIODirectionInOut | kIOMemoryMapperNone |
            ((a->fCache >> kIOMapCacheShift) << 28), nullptr)) {
        fLease.reset(); return false;
    }
    return true;
}
IOMemoryMap *NVVramMemoryDescriptor::makeMapping(IOMemoryDescriptor *owner, task_t task, IOVirtualAddress address, IOOptionBits options, IOByteCount offset, IOByteCount length)
{
    NVVramAllocation *a = fLease.get();
    const IOOptionBits cache = options & kIOMapCacheMask;
    if (!a || !a->fOwner->active() || (cache != kIOMapDefaultCache && cache != a->fCache)) {
        if (address) ((IOMemoryMap *)address)->release();
        return nullptr;
    }
    return IOGeneralMemoryDescriptor::makeMapping(owner, task, address,
        (options & ~kIOMapCacheMask) | a->fCache, offset, length);
}
void NVVramMemoryDescriptor::free()
{
    // Release the descriptor's mapping state before allowing the allocation to retire.
    NVVramAllocation *a = fLease.take();
    IOGeneralMemoryDescriptor::free();
    if (a) { a->releaseLease(NVLeaseUse::CpuAccess); a->release(); }
}

extern "C" void _ZN18IOAccelDisplayPipe28destroy_framebuffer_resourceEP16IOAccelResource2(IOAccelDisplayPipe *, IOAccelResource2 *);
extern "C" mach_vm_address_t _ZN16IOAccelResource216lockForCPUAccessEP4task9eLockTypejbhhPi(
    IOAccelResource2 *, task_t, unsigned int, unsigned int, bool, unsigned char, unsigned char, int *);
extern "C" void _ZN16IOAccelResource218unlockForCPUAccessEP4task9eLockType(IOAccelResource2 *, task_t, unsigned int);

IOAccelMemory *NVDisplayPipe::initFramebufferResource(unsigned int index, IOAccelResource2 *resource)
{
    (void)index;
    NVResource *res = OSDynamicCast(NVResource, resource);
    NVAccel *a = fAccel;
    if (!gNvScanout || !a || a->fStopping || !res || fFramebufferResource) return nullptr;
    IOService *fb = a->pipeFramebuffer(this, nullptr);
    if (!fb) return nullptr;
    uint64_t bytes = 0;
    if (fb->metaCast("NMIntelFB")) {
        NMIntelFramebufferInfo info = {}; info.version = NM_INTEL_FB_ABI_VERSION; info.structSize = sizeof(info);
        const OSSymbol *s = OSSymbol::withCStringNoCopy(NM_INTEL_FB_INFO_FN);
        IOReturn status = s ? fb->callPlatformFunction(s, false, &info, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
        if (s) s->release();
        bytes = (uint64_t)info.pitch * info.height;
        if (!a->fCandidate || status != kIOReturnSuccess || info.framebufferRegistryID != fb->getRegistryEntryID() ||
            !info.epoch || !info.width || !info.height || (uint64_t)info.width * 4 > info.pitch ||
            !bytes || bytes > info.actualSize || info.format != NM_INTEL_FB_FORMAT_BGRA) return nullptr;
        fFramebufferInfo = info;
    } else {
        NVRMVramRequest q = {}; q.version = NVRM_VRAM_ABI_VERSION;
        if (a->askFramebufferForScanout(&q, fb) != kIOReturnSuccess) return nullptr;
        bytes = (uint64_t)q.pitch * q.height;
        if (!bytes || bytes > q.actualSize || !q.width || (uint64_t)q.width * 4 > q.pitch) return nullptr;
    }
    const uint64_t rounded = nm_optc_align_vram_size(bytes);
    if (!rounded) return nullptr;
    OSObject *object = a->createVidMemory(nullptr, res, rounded, nullptr);
    NVVidMemory *memory = OSDynamicCast(NVVidMemory, object);
    if (!memory) { if (object) object->release(); return nullptr; }
    memory->fScanout = false;
    res->fFramebufferPipe = this;
    fFramebufferResource = res;
    fFramebufferPhase = NVFramebufferPhase::Candidate;
    return memory;
}

static NVVidMemory *nvres_vidmem(IOAccelResource2 *r)
{
    if (!r) return nullptr;
    OSObject *o = *(OSObject **)((unsigned char *)r + kNvResVidMem);
    return OSDynamicCast(NVVidMemory, o);
}

bool NVResource::addToAperture()
{
    NVVidMemory *m = nvres_vidmem(this);
    return m && m->fBacking && m->fOwner->active();
}
void NVResource::removeFromAperture()
{
    if (fApDesc) { fApDesc->release(); fApDesc = nullptr; }
    fApertureKey = {};
}
void *NVResource::getApertureMemoryDescriptor(unsigned long long *offset)
{
    if (offset) *offset = 0;
    NVVidMemory *m = nvres_vidmem(this);
    if ((!gNvAperture && !fFramebufferPipe) || !m || !m->fBacking || !m->fOwner->active()) return nullptr;
    const NVBackingKey key = m->fBacking->key();
    if (fApDesc && !nvBackingKeyEqual(fApertureKey, key)) removeFromAperture();
    if (!fApDesc) {
        NVVramMemoryDescriptor *descriptor = OSTypeAlloc(NVVramMemoryDescriptor);
        if (!descriptor || !descriptor->initWithAllocation(m->fBacking, 0, m->capacity())) {
            if (descriptor) descriptor->release();
            return nullptr;
        }
        fApDesc = descriptor; fApertureKey = key;
    }
    return fApDesc;
}
bool NVResource::prepare()
{
    NVVramOwner *owner = fAccel ? fAccel->fOwner : nullptr;
    const bool admitted = owner && owner->enter();
    struct Operation { NVVramOwner *owner; ~Operation() { if (owner) owner->leave(); } } operation = { admitted ? owner : nullptr };
    NVDisplayPipe *pipe = fFramebufferPipe;
    if (!pipe) return admitted && IOAccelResource2::prepare();
    const bool initial = pipe->fFramebufferPhase == NVFramebufferPhase::Candidate && !fSuccessfulPrepares;
    const bool prepared = admitted && fSuccessfulPrepares != UINT32_MAX && IOAccelResource2::prepare();
    if (prepared) ++fSuccessfulPrepares;
    if (!initial) return prepared;
    if (prepared) {
        fFramebufferMemory = nvres_vidmem(this);
        if (fFramebufferMemory) {
            fFramebufferMemory->retain();
            fMemoryPinned = fFramebufferMemory->prepare();
            if (fMemoryPinned && fFramebufferMemory->fBacking &&
                (!pipe->fFramebufferInfo.actualSize ||
                 (uint64_t)pipe->fFramebufferInfo.pitch * pipe->fFramebufferInfo.height <= fFramebufferMemory->capacity()) &&
                getApertureMemoryDescriptor(nullptr)) {
                pipe->fFramebufferPhase = NVFramebufferPhase::Ready;
                return true;
            }
        }
    }
    // 25G342 adopts memory before prepare, but does not unwind a failed prepare.
    // Suppress exactly the native destructor's unmatched completion, and nothing else.
    fAbortWithoutPrepare = !prepared;
    _ZN18IOAccelDisplayPipe28destroy_framebuffer_resourceEP16IOAccelResource2(pipe, this);
    fAbortWithoutPrepare = false;
    return false;
}
void NVResource::complete()
{
    if (fAbortWithoutPrepare) return;
    IOAccelResource2::complete();
    if (fSuccessfulPrepares) --fSuccessfulPrepares;
}
void NVResource::releaseFramebufferBacking()
{
    removeFromAperture();
    if (fFramebufferMemory) {
        if (fMemoryPinned) fFramebufferMemory->complete();
        fMemoryPinned = false;
        fFramebufferMemory->release(); fFramebufferMemory = nullptr;
    }
    fCpuPublication = 0; fCpuPublicationKey = {};
    fFramebufferPipe = nullptr;
}
void NVDisplayPipe::destroyFramebufferResource(unsigned int index, IOAccelResource2 *resource)
{
    (void)index;
    NVResource *res = OSDynamicCast(NVResource, resource);
    if (res) res->releaseFramebufferBacking();
    if (resource == fFramebufferResource) {
        fFramebufferResource = nullptr; fFramebufferInfo = {};
        fFramebufferPhase = NVFramebufferPhase::Empty;
        dropHostFrameBuffer();
    }
}
void NVDisplayPipe::displayModeWillChange()
{
    fFramebufferPhase = NVFramebufferPhase::Retiring;
    nvAccelDrainPipe(this);
    if (fTransactionLock) { IOLockLock(fTransactionLock); fTransaction.begin(nullptr, 0, 0, 0); IOLockUnlock(fTransactionLock); }
    IOAccelLegacyDisplayPipe::displayModeWillChange();
    if (fFramebufferResource) {
        NVResource *res = fFramebufferResource;
        if (res->fSuccessfulPrepares)
            _ZN18IOAccelDisplayPipe28destroy_framebuffer_resourceEP16IOAccelResource2(this, res);
        else destroyFramebufferResource(0, res);
    }
}

static void nvSVForgetResource(NVResource *resource);

void NVResource::free()
{
    nvSVForgetResource(this);
    if (fFramebufferPipe) fFramebufferPipe->destroyFramebufferResource(0, this);
    else releaseFramebufferBacking();
    OSAddAtomic64(1, &gCntResFree);
    IOAccelResource2::free();
}

// 1 = a surface keeps its VRAM across the family's deallocPhysical/allocPhysical (see deallocPhysical).
static int gVmSticky = 1;
static SInt64 gCntVmParked, gCntVmUnparked, gCntVmParkReleased;

bool NVVidMemory::bindOwner(NVVramOwner *owner)
{
    if (fOwner || !owner || !owner->borrow()) return false;
    fOwner = owner; return true;
}
bool NVVidMemory::allocPhysical()
{
    if (fDestroying || !fOwner || !fOwner->active() || fBacking) return false;
    const uint64_t want = getLength();
    if (!want) return false;
    if (fParked && fParked->fReceipt.actualSize >= want && fParked->unpark()) {
        fBacking = fParked; fParked = nullptr; OSAddAtomic64(1, &gCntVmUnparked);
    } else {
        releaseParked();
        fBacking = fOwner->allocate(want);
        if (!fBacking) { OSAddAtomic64(1, &gCntVmRefused); return false; }
        OSAddAtomic64(1, &gCntVmAlloc); OSAddAtomic64((SInt64)capacity(), &gCntVmAllocBytes);
    }
    *(volatile unsigned long long *)((unsigned char *)this + kNvVidMemPhysOff) = physical();
    return true;
}
void NVVidMemory::deallocPhysical()
{
    if (!fBacking) return;
    NVVramAllocation *old = fBacking; fBacking = nullptr;
    OSAddAtomic64(1, &gCntVmDealloc); OSAddAtomic64((SInt64)old->fReceipt.actualSize, &gCntVmDeallocBytes);
    if (!fDestroying && gVmSticky && old->park()) {
        releaseParked(); fParked = old; OSAddAtomic64(1, &gCntVmParked);
    } else { old->retire(); old->release(); }
    *(volatile unsigned long long *)((unsigned char *)this + kNvVidMemPhysOff) = 0;
}
void NVVidMemory::releaseParked()
{
    if (!fParked) return;
    fParked->retire(); fParked->release(); fParked = nullptr;
    OSAddAtomic64(1, &gCntVmParkReleased);
}
uint64_t NVVidMemory::getPhysicalSegment(unsigned long long offset, unsigned long long *length)
{
    const uint64_t bytes = capacity();
    if (!fOwner || !fOwner->active() || !physical() || offset >= bytes) { if (length) *length = 0; return 0; }
    if (length) *length = bytes - offset;
    return physical() + offset;
}
void NVVidMemory::free()
{
    fDestroying = true;
    releaseParked();
    NVVramOwner *owner = fOwner;
    // The family unwires first and calls our deallocPhysical; it owns the live accounting.
    IOAccelVidMemory::free();
    if (owner) owner->unborrow();
}

#if defined(NM_ERA) && NM_ERA < 1015
// macOS 10.13 and 10.14: the IOSurface kext exports no id, size or format accessors (drivers read the object), and its
// memory-descriptor accessor is not const. The fields are where 10.15's own accessors read them (id +0xc, width +0x90,
// height +0x94, bytes per row +0x98, pixel format +0xa4); every 10.14 method that touches them on 10.15 still does.
extern "C" void *_ZN9IOSurface19getMemoryDescriptorEv(void *surface);
class IOSurface {
    template <typename T> T nmField(unsigned off) const { return *(const volatile T *)((const unsigned char *)this + off); }
public:
    IOMemoryDescriptor *getMemoryDescriptor() const { return (IOMemoryDescriptor *)_ZN9IOSurface19getMemoryDescriptorEv((void *)this); }
    unsigned long long  getWidth() const { return nmField<unsigned int>(0x90); }
    unsigned long long  getHeight() const { return nmField<unsigned int>(0x94); }
    unsigned long long  getBytesPerRow() const { return nmField<unsigned int>(0x98); }
    unsigned int        getPixelFormat() const { return nmField<unsigned int>(0xa4); }
    unsigned int        getSurfaceID() const { return nmField<unsigned int>(0xc); }
};
#else
class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
};
#endif
class IOAccelDisplayPipeTransaction2 {
public:
    IOSurface *getPlaneIOSurface(unsigned int, unsigned int) const;
    OSObject  *getPlaneResource(unsigned int, unsigned int) const;
    unsigned int getTransactionID() const;
};

static SInt64 gCntIopOk, gCntIopFail, gCntIopEmpty, gCntIopSrcRes, gCntIopSrcSurf, gCntIopBlank;
static SInt64 gCntIopOkHead[kNvMaxFB];
static SInt64 gCntIop30, gCntIopHalf;       // frames written to a 30-bit scanout; frames converted from half-float RGhA
static SInt64 gCntIopL10r;                 // frames narrowed from 10-bit l10r (login window, lock screen)
// hybrid laptops (nm_hybrid.h)
static SInt64 gCntHyFrames, gCntHyFlips, gCntHyImportFail, gCntHyOwned, gCntHyDma, gCntHyDmaTimeout, gCntHyP2p;
#define NVIOP_LOG_MAX 40
static unsigned gIopLogs = 0;
#define NVIOPLOG(fmt, ...) do { if (gIopLogs < NVIOP_LOG_MAX) { gIopLogs++; ALOG("iop: " fmt, ##__VA_ARGS__); } } while (0)
#define NVIOP_FAIL(rc, fmt, ...) do { OSAddAtomic64(1, &gCntIopFail); NVIOPLOG(fmt, ##__VA_ARGS__); return (rc); } while (0)

void NVDisplayPipe::iopDropSources()
{
    for (unsigned i = 0; i < kIopSrcSlots; i++) {
        NVIopSrc *e = &fIopSrc[i];
        if (e->map) { e->map->release(); e->map = nullptr; }
        if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
        e->id = 0;
    }
}

void NVDisplayPipe::dropHostFrameBuffer()
{
    fHostFrameKva = nullptr;
    fHostFrameBytes = 0;
    if (fHostFrameBuffer) { fHostFrameBuffer->release(); fHostFrameBuffer = nullptr; }
}

bool NVDisplayPipe::ensureHostFrameBuffer(uint64_t bytes)
{
    if (!bytes) return false;
    if (fHostFrameBuffer && fHostFrameBytes >= bytes && fHostFrameKva) return true;
    dropHostFrameBuffer();
    IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, (mach_vm_size_t)bytes, PAGE_SIZE);
    if (!buf) return false;
    void *kva = buf->getBytesNoCopy();
    if (!kva || buf->getLength() < bytes) { buf->release(); return false; }
    fHostFrameBuffer = buf;
    fHostFrameKva = kva;
    fHostFrameBytes = buf->getLength();
    return true;
}

void NVDisplayPipe::free()
{
    if (fFramebufferResource) displayModeWillChange();
    iopDropSources();
    dropHostFrameBuffer();
    IOLock *lock = fTransactionLock; fTransactionLock = nullptr;
    if (lock) IOLockFree(lock);
    IOAccelLegacyDisplayPipe::free();
}

const void *NVDisplayPipe::iopMapSurface(unsigned id, IOMemoryDescriptor *md, unsigned long long need, bool *cached)
{
    if (cached) *cached = false;
    for (unsigned i = 0; i < kIopSrcSlots; i++)
        if (fIopSrc[i].md == md && fIopSrc[i].id == id && fIopSrc[i].map) {
            if (fIopSrc[i].map->getLength() < need) return nullptr;
            if (cached) *cached = fIopSrc[i].cached;
            return (const void *)fIopSrc[i].map->getVirtualAddress();
        }
    if (md->getLength() < need) return nullptr;
    NVIopSrc *e = &fIopSrc[fIopNext % kIopSrcSlots]; fIopNext++;
    if (e->map) { e->map->release(); e->map = nullptr; }
    if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
    e->id = 0; e->cached = false;
    if (md->prepare() != kIOReturnSuccess) return nullptr;
    IOMemoryMap *m = md->map(kIOMapReadOnly);
    if (!m) { md->complete(); return nullptr; }
    md->retain(); e->md = md; e->map = m; e->id = id;
    const uint32_t c = (uint32_t)(m->getMapOptions() & kIOMapCacheMask);
    e->cached = (c != kIOMapInhibitCache && c != kIOMapWriteCombineCache);
    if (cached) *cached = e->cached;
    NVIOPLOG("mapped source surface 0x%x (%llu bytes) into slot %u (cached=%d opts=0x%x)",
             id, (unsigned long long)m->getLength(), (fIopNext - 1) % kIopSrcSlots,
             e->cached ? 1 : 0, (unsigned)m->getMapOptions());
    return (const void *)m->getVirtualAddress();
}

IOReturn NVDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    if (!txn) return kIOReturnBadArgument;
    // 25G342 prepare@0x4af8a traverses exactly two planes, each with two images.
    for (unsigned plane = 0; plane < 2; ++plane)
        for (unsigned image = 0; image < 2; ++image)
            if ((plane || image) && (txn->getPlaneIOSurface(plane, image) || txn->getPlaneResource(plane, image)))
                return kIOReturnUnsupported;
    IOSurface *s = txn->getPlaneIOSurface(0, 0);
    if (s) {
        const unsigned fmt = s->getPixelFormat();
        if (fmt != NV_IOP_FMT_BGRA && fmt != NV_IOP_FMT_L10R && fmt != NV_IOP_FMT_RGHA) return kIOReturnUnsupported;
        const uint64_t w = s->getWidth(), h = s->getHeight(), pitch = s->getBytesPerRow();
        if (!w || !h || w > UINT32_MAX || h > UINT32_MAX || pitch > UINT32_MAX ||
            pitch < w * NV_IOP_BYTES_PER_PIXEL(fmt) || (pitch & 3u)) return kIOReturnBadArgument;
    } else if (OSObject *object = txn->getPlaneResource(0, 0)) {
        if (!fAccel) return kIOReturnNotReady;
        const bool owned = fAccel->familyOwned();
        if (!owned && !nvFamLock(fAccel, true)) return kIOReturnNotReady;
        NVResource *r = OSDynamicCast(NVResource, object);
        NVVidMemory *m = r ? nvres_vidmem(r) : nullptr;
        const bool published = r && m && m->fBacking && r->fCpuPublication && nvResPitch((const unsigned char *)r) <= UINT32_MAX &&
            nvBackingKeyEqual(r->fCpuPublicationKey, m->fBacking->key());
        if (!owned) nvFamUnlock(fAccel);
        if (!published) return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}
bool NVDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *txn)
{
    if (!txn || !fTransactionLock) return false;
    IOLockLock(fTransactionLock);
    bool complete = fTransaction.matches(txn, txn->getTransactionID());
    IOLockUnlock(fTransactionLock);
    return complete;
}
void NVDisplayPipe::finishTransaction(const NVTransactionState &submitted, IOReturn result)
{
    if (!fTransactionLock) return;
    IOLockLock(fTransactionLock);
    fTransaction.finish(submitted.transaction, submitted.sequence, submitted.epoch, submitted.key, (int)result, true);
    IOLockUnlock(fTransactionLock);
}

static bool nvAccelSurfCopyReady(IOSurface *s, unsigned head);
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe);
static bool nvSurfWaitReady(unsigned int id);
static bool nvAccelHybridImport(IOService *fb, unsigned head, IOSurface *s, NVVramAllocation *vram);
static bool nvAccelHybridDma(NVDisplayPipe *pipe, IOService *fb, unsigned head, IOSurface *s);
static bool nvAccelHybridOwned(IOService *fb, unsigned head, const volatile unsigned int *src, unsigned w, unsigned h, unsigned pitch, bool from10, unsigned dpitch);
static bool nvSVPend(unsigned id, int op, NVResource *resource);
static bool nvAccelFlatHeld(NVDisplayPipe *pipe, unsigned head, IOSurface *s);
static bool nvAccelHybridHome(IOService *fb, unsigned head);
static bool nvAccelIopHome(IOService *fb, unsigned head);
struct NVLegacyCopy {
    nv_frame_span source, destination;
    bool dst30, deviceSink;
    NVVramOwner *owner;
};
static IOReturn nvLegacyCopyBody(void *context)
{
    NVLegacyCopy *c = (NVLegacyCopy *)context;
    if (c->deviceSink) {
        NVRMKernelMapInfo q = { NVRM_OWNER_ABI_VERSION, sizeof(q), c->destination.address,
            (uint64_t)c->destination.pitch * c->destination.height, 0, 0, 0, 0 };
        if (c->owner->call(NVRM_KERNEL_MAP_INFO_FN, &q) != kIOReturnSuccess ||
            q.availableBytes < c->destination.capacity ||
            (q.cacheOptions != kIOMapInhibitCache && q.cacheOptions != kIOMapWriteCombineCache)) return kIOReturnVMError;
    }
    if (nv_frame_validate(&c->source) != NV_FRAME_OK || nv_frame_validate(&c->destination) != NV_FRAME_OK ||
        c->source.width != c->destination.width || c->source.height != c->destination.height) return kIOReturnOverrun;
    for (unsigned y = 0; y < c->source.height; ++y)
        (void)nv_iop_convert_row((volatile unsigned *)((unsigned char *)(uintptr_t)c->destination.address + (size_t)y * c->destination.pitch),
            (const volatile unsigned *)((const unsigned char *)c->source.address + (size_t)y * c->source.pitch),
            c->source.width, c->source.format, c->dst30 ? 1 : 0);
    nv_frame_store_drain();
    return kIOReturnSuccess;
}
IOReturn NVDisplayPipe::performLegacyTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    NVAccel *a = fAccel;
    unsigned head = 0;
    IOService *fb = a ? a->pipeFramebuffer(this, &head) : nullptr;
    IOSurface *s = txn ? txn->getPlaneIOSurface(0, 0) : nullptr;
    if (!a || !fb || !s || fb->metaCast("NMIntelFB")) return kIOReturnUnsupported;
    if (a->fOwner->scanoutRetiring(head)) return kIOReturnNotReady;
    if (!nvSurfWaitReady(s->getSurfaceID())) return kIOReturnTimeout;
    if (gNmFlatSkip && head < 5 && fb->metaCast("NVRMNVDAFramebuffer") && nvAccelFlatHeld(this, head, s)) {
        OSAddAtomic64(1, &gCntFlatHeld); return kIOReturnNotReady;
    }
    if (nvAccelIopFlip(fb, head, s, this)) {
        IOLockLock(fTransactionLock); bool queued = fTransaction.queued; IOLockUnlock(fTransactionLock);
        if (!queued) { OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]); }
        return kIOReturnSuccess;
    }
    if (gNmHybrid >= NMH_DMA && nvAccelHybridDma(this, fb, head, s)) {
        OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]); return kIOReturnSuccess;
    }
    NVVramLease sourceAllocation;
    if (gNmHybridP2P && nvFamLock(a, true)) {
        NVResource *r = OSDynamicCast(NVResource, txn->getPlaneResource(0, 0));
        NVVidMemory *m = r ? nvres_vidmem(r) : nullptr;
        if (m && m->fBacking) m->fBacking->acquire(NVLeaseUse::Present, &sourceAllocation);
        nvFamUnlock(a);
    }
    if (gNmHybrid >= NMH_IMPORT && nvAccelHybridImport(fb, head, s, sourceAllocation.get())) {
        OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]); return kIOReturnSuccess;
    }
    if (!nvAccelSurfCopyReady(s, head)) return kIOReturnNotReady;
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    const uint64_t w = s->getWidth(), h = s->getHeight(), pitch = s->getBytesPerRow();
    if (!md || !w || !h || w > UINT32_MAX || h > UINT32_MAX || pitch > UINT32_MAX) return kIOReturnBadArgument;
    nv_frame_span source = { iopMapSurface(s->getSurfaceID(), md, h * pitch), md->getLength(),
        (uint32_t)w, (uint32_t)h, (uint32_t)pitch, s->getPixelFormat() };
    if (nv_frame_validate(&source) != NV_FRAME_OK) return kIOReturnOverrun;
    NVRMVramRequest q = {}; q.version = NVRM_VRAM_ABI_VERSION;
    if (a->askFramebufferForScanout(&q, fb) != kIOReturnSuccess) return kIOReturnNotReady;
    NVLegacyCopy copy = { source, { q.kva, q.actualSize, q.width, q.height, q.pitch, NV_IOP_FMT_BGRA },
        (q.flags & NVRM_VRAM_SCANOUT_30BIT) != 0, fb->metaCast("NVRMNVDAFramebuffer") != nullptr, a->fOwner };
    if (nv_frame_validate(&copy.destination) != NV_FRAME_OK || q.width != w || q.height != h) return kIOReturnOverrun;
    if (!nvAccelIopHome(fb, head)) return kIOReturnNotReady;
    IOReturn result;
    if (gNmHybrid >= NMH_OWNED && !q.phys && !copy.dst30 &&
        (source.format == NV_IOP_FMT_BGRA || source.format == NV_IOP_FMT_L10R) &&
        nvAccelHybridOwned(fb, head, (const volatile unsigned *)source.address, source.width, source.height,
            source.pitch, source.format == NV_IOP_FMT_L10R, q.pitch)) {
        OSAddAtomic64(1, &gCntHyOwned); result = kIOReturnSuccess;
    } else {
        if (!fb->metaCast("NVRMNVDAFramebuffer") && !nvAccelHybridHome(fb, head)) return kIOReturnNotReady;
        result = a->fOwner->cpuAccess(nvLegacyCopyBody, &copy);
    }
    if (result == kIOReturnSuccess) {
        OSAddAtomic64(1, &gCntIopSrcSurf); OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]);
        if (copy.dst30) OSAddAtomic64(1, &gCntIop30);
        if (source.format == NV_IOP_FMT_L10R) OSAddAtomic64(1, &gCntIopL10r);
        if (source.format == NV_IOP_FMT_RGHA) OSAddAtomic64(1, &gCntIopHalf);
    }
    return result;
}

static SInt64 gCntPresentProducer, gCntPresentPublication, gCntPresentBounds, gCntPresentEpoch, gCntPresentCopy;
struct NVFinalTransfer {
    NVDisplayPipe *pipe;
    IOService *framebuffer;
    NVResource *resource;
    NVVramAllocation *allocation;
    nv_frame_span source;
    nv_frame_span hostStage;
    uint64_t sequence;
    bool sourceCached;
};
static IOReturn nvPopulateAndPresentFinal(void *context)
{
    NVFinalTransfer *x = (NVFinalTransfer *)context;
    const uint32_t cache = x->allocation->fCache;
    if (cache != kIOMapInhibitCache && cache != kIOMapWriteCombineCache) return kIOReturnUnsupported;
    int error = 0;
    const unsigned mapOptions = cache == kIOMapWriteCombineCache ? 0x401u : 0x101u;
    mach_vm_address_t address = _ZN16IOAccelResource216lockForCPUAccessEP4task9eLockTypejbhhPi(
        x->resource, kernel_task, 2, mapOptions, false, 0, 0, &error);
    if (!address) return kIOReturnVMError;
    struct Unlock {
        NVResource *resource;
        ~Unlock() { _ZN16IOAccelResource218unlockForCPUAccessEP4task9eLockType(resource, kernel_task, 2); }
    } unlock = { x->resource };
    IOMemoryMap *map = nvRes25G342KernelCPUMap(x->resource);
    NVVramMemoryDescriptor *descriptor = map ? OSDynamicCast(NVVramMemoryDescriptor, map->getMemoryDescriptor()) : nullptr;
    if (!map || !descriptor || descriptor->allocation() != x->allocation ||
        !nvBackingKeyEqual(x->allocation->key(), x->resource->fApertureKey) ||
        map->getAddressTask() != kernel_task || map->getVirtualAddress() != address ||
        (map->getMapOptions() & kIOMapCacheMask) != cache) return kIOReturnNotPermitted;
    const bool intel = x->framebuffer->metaCast("NMIntelFB") != nullptr;
    NMIntelFramebufferInfo info = x->pipe->fFramebufferInfo;
    NVRMVramRequest scanout = {}; scanout.version = NVRM_VRAM_ABI_VERSION;
    if (!intel) {
        if (x->pipe->fAccel->askFramebufferForScanout(&scanout, x->framebuffer) != kIOReturnSuccess) return kIOReturnNotReady;
        info.width = scanout.width; info.height = scanout.height; info.pitch = scanout.pitch;
        info.actualSize = scanout.actualSize; info.format = NM_INTEL_FB_FORMAT_BGRA;
    }
    if (!info.actualSize || (uint64_t)info.pitch * info.height > info.actualSize ||
        nvResPitch((const unsigned char *)x->resource) != info.pitch) return kIOReturnOverrun;
    nv_frame_span target = { (const void *)(uintptr_t)address, map->getLength(), info.width, info.height, info.pitch, NV_IOP_FMT_BGRA };
    if (target.capacity > x->allocation->fReceipt.actualSize) target.capacity = x->allocation->fReceipt.actualSize;
    nv_frame_span presentSource = x->source;
    if (!x->sourceCached || x->source.format != NV_IOP_FMT_BGRA) {
        if (!x->hostStage.address || nv_frame_populate(&x->source, &x->hostStage) != NV_FRAME_OK) return kIOReturnOverrun;
        presentSource = x->hostStage;
    }
    if (nv_frame_populate(&presentSource, &target) != NV_FRAME_OK) return kIOReturnOverrun;
    nv_frame_store_drain();
    const NVBackingKey key = x->allocation->key();
    x->resource->fCpuPublicationKey = key; x->resource->fCpuPublication = x->sequence;
    if (intel) {
        NMIntelPresentRequest q = {};
        q.version = NM_INTEL_FB_ABI_VERSION; q.structSize = sizeof(q);
        q.framebufferRegistryID = info.framebufferRegistryID; q.epoch = info.epoch;
        q.sourceOwnerGeneration = key.ownerGeneration; q.sourceAllocationCookie = key.allocationGeneration;
        q.sequence = x->sequence; q.sourceKva = presentSource.address; q.sourceBytes = presentSource.capacity;
        q.width = presentSource.width; q.height = presentSource.height; q.pitch = presentSource.pitch; q.format = presentSource.format;
        const OSSymbol *symbol = OSSymbol::withCStringNoCopy(NM_INTEL_FB_PRESENT_FN);
        IOReturn result = symbol ? x->framebuffer->callPlatformFunction(symbol, false, &q, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
        if (symbol) symbol->release();
        if (result != kIOReturnSuccess) return result;
        if (q.completedSequence != q.sequence) return kIOReturnIOError;
        OSAddAtomic64(1, &gCntHyFrames);
    } else {
        // Stock NVRMFB owns the scanout. Its mode may change between calls, but fMem is stable.
        NVRMKernelMapInfo mi = { NVRM_OWNER_ABI_VERSION, sizeof(mi), scanout.kva, (uint64_t)scanout.pitch * scanout.height, 0, 0, 0, 0 };
        if (x->allocation->fOwner->call(NVRM_KERNEL_MAP_INFO_FN, &mi) != kIOReturnSuccess ||
            mi.availableBytes < scanout.actualSize ||
            (mi.cacheOptions != kIOMapInhibitCache && mi.cacheOptions != kIOMapWriteCombineCache)) return kIOReturnVMError;
        nv_frame_span sink = { scanout.kva, scanout.actualSize, scanout.width, scanout.height, scanout.pitch, NV_IOP_FMT_BGRA };
        if (nv_frame_validate(&sink) != NV_FRAME_OK) return kIOReturnOverrun;
        if (scanout.flags & NVRM_VRAM_SCANOUT_30BIT) {
            for (unsigned y = 0; y < sink.height; ++y)
                (void)nv_iop_convert_row((volatile unsigned *)((unsigned char *)scanout.kva + (size_t)y * sink.pitch),
                    (const volatile unsigned *)((const unsigned char *)presentSource.address + (size_t)y * presentSource.pitch), sink.width, NV_IOP_FMT_BGRA, 1);
        } else if (nv_frame_populate(&presentSource, &sink) != NV_FRAME_OK) return kIOReturnOverrun;
        nv_frame_store_drain();
    }
    return kIOReturnSuccess;
}
IOReturn NVDisplayPipe::presentFinal(IOService *fb, const nv_frame_span &source, bool sourceCached)
{
    if (!fAccel || fAccel->fStopping) return kIOReturnNotReady;
    nv_frame_span hostStage = {};
    if (!sourceCached || source.format != NV_IOP_FMT_BGRA) {
        if (!source.width || !source.height || source.width > 16384u || source.height > 16384u) return kIOReturnOverrun;
        const uint64_t hostPitch = ((uint64_t)source.width * 4u + 7ull) & ~7ull;
        if (hostPitch > UINT32_MAX || hostPitch > UINT64_MAX / source.height) return kIOReturnOverrun;
        const uint64_t hostBytes = hostPitch * source.height;
        if (!ensureHostFrameBuffer(hostBytes)) return kIOReturnNoMemory;
        hostStage = { fHostFrameKva, hostBytes, source.width, source.height, (uint32_t)hostPitch, NV_IOP_FMT_BGRA };
    }
    if (!nvFamLock(fAccel, true)) return kIOReturnNotReady;
    IOReturn result = kIOReturnNotReady;
    NVVramLease lease;
    NVResource *r = fFramebufferResource;
    NVVidMemory *m = r ? r->fFramebufferMemory : nullptr;
    if (fFramebufferPhase == NVFramebufferPhase::Ready && m && m->fBacking &&
        m->fBacking->acquire(NVLeaseUse::Present, &lease)) {
        IOLockLock(fTransactionLock); const uint64_t sequence = fTransaction.sequence; IOLockUnlock(fTransactionLock);
        NVFinalTransfer x = { this, fb, r, lease.get(), source, hostStage, sequence, sourceCached };
        result = m->fOwner->cpuAccess(nvPopulateAndPresentFinal, &x);
        IOLockLock(fTransactionLock);
        if (fTransaction.sequence == sequence) fTransaction.key = lease.get()->key();
        IOLockUnlock(fTransactionLock);
    }
    nvFamUnlock(fAccel);
    return result;
}
IOReturn NVDisplayPipe::performCandidateTransaction(IOAccelDisplayPipeTransaction2 *txn, IOService *fb, unsigned head)
{
    IOSurface *surface = txn->getPlaneIOSurface(0, 0);
    nv_frame_span source = {};
    NVVramLease resourceLease;
    bool sourceCached = false;
    if (surface) {
        if (!nvSurfWaitReady(surface->getSurfaceID())) { OSAddAtomic64(1, &gCntPresentProducer); return kIOReturnTimeout; }
        if (!nvAccelSurfCopyReady(surface, head)) { OSAddAtomic64(1, &gCntPresentPublication); return kIOReturnNotReady; }
        IOMemoryDescriptor *md = surface->getMemoryDescriptor();
        if (!md) return kIOReturnVMError;
        source.width = (uint32_t)surface->getWidth(); source.height = (uint32_t)surface->getHeight();
        source.pitch = (uint32_t)surface->getBytesPerRow(); source.format = surface->getPixelFormat();
        source.capacity = md->getLength();
        source.address = iopMapSurface(surface->getSurfaceID(), md, (uint64_t)source.pitch * source.height, &sourceCached);
    } else {
        if (!nvFamLock(fAccel, true)) return kIOReturnNotReady;
        NVResource *r = OSDynamicCast(NVResource, txn->getPlaneResource(0, 0));
        NVVidMemory *m = r ? nvres_vidmem(r) : nullptr;
        bool ok = m && m->fBacking && r->fCpuPublication &&
            nvBackingKeyEqual(r->fCpuPublicationKey, m->fBacking->key()) && m->fBacking->acquire(NVLeaseUse::CpuAccess, &resourceLease);
        if (ok) {
            source = { m->kva(), m->capacity(), *(const uint16_t *)((const char *)r + kNvResWidth),
                *(const uint16_t *)((const char *)r + kNvResHeight), (uint32_t)nvResPitch((const unsigned char *)r), NV_IOP_FMT_BGRA };
        }
        nvFamUnlock(fAccel);
        if (!ok) return kIOReturnUnsupported;
    }
    if (nv_frame_validate(&source) != NV_FRAME_OK) { OSAddAtomic64(1, &gCntPresentBounds); return kIOReturnOverrun; }
    IOReturn result = presentFinal(fb, source, sourceCached);
    if (result == kIOReturnNotReady) OSAddAtomic64(1, &gCntPresentEpoch);
    else if (result != kIOReturnSuccess) OSAddAtomic64(1, &gCntPresentCopy);
    return result;
}
struct NVTransactionGate {
    NVDisplayPipe *pipe;
    bool admitted = false;
    explicit NVTransactionGate(NVDisplayPipe *p) : pipe(p) {
        if (!pipe || !pipe->fTransactionLock) return;
        IOLockLock(pipe->fTransactionLock);
        admitted = !pipe->fTransactionBusy;
        if (admitted) pipe->fTransactionBusy = true;
        IOLockUnlock(pipe->fTransactionLock);
    }
    ~NVTransactionGate() {
        if (!admitted) return;
        IOLockLock(pipe->fTransactionLock); pipe->fTransactionBusy = false; IOLockUnlock(pipe->fTransactionLock);
    }
};
IOReturn NVDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    if (!gNvIop || !fAccel || fAccel->fStopping) return kIOReturnNotReady;
    NVVramOwner *owner = fAccel->fOwner;
    if (!owner || !owner->enter()) return kIOReturnNotReady;
    struct Operation { NVVramOwner *owner; ~Operation() { owner->leave(); } } operation = { owner };
    NVTransactionGate gate(this);
    if (!gate.admitted) return kIOReturnBusy;
    IOReturn result = validateTransaction(txn);
    if (result != kIOReturnSuccess) return result;
    uint64_t sequence = 0;
    IOLockLock(fTransactionLock);
    bool numbered = nvNextGeneration(&fNextSequence, &sequence);
    if (numbered) fTransaction.begin(txn, txn->getTransactionID(), sequence, fFramebufferInfo.epoch);
    NVTransactionState submitted = fTransaction;
    IOLockUnlock(fTransactionLock);
    if (!numbered) return kIOReturnNoResources;
    if (!txn->getPlaneIOSurface(0, 0) && !txn->getPlaneResource(0, 0)) {
        IOLockLock(fTransactionLock);
        fTransaction.finish(txn, sequence, fFramebufferInfo.epoch, {}, 0, false);
        IOLockUnlock(fTransactionLock);
        OSAddAtomic64(1, &gCntIopEmpty); return kIOReturnSuccess;
    }
    unsigned head = 0; IOService *fb = fAccel->pipeFramebuffer(this, &head);
    if (!fb || fb->isInactive()) return kIOReturnNotReady;
    fb->retain(); NVRetain retained(fb);
    const bool finalPath = fAccel->fCandidate || !txn->getPlaneIOSurface(0, 0);
    result = finalPath ? performCandidateTransaction(txn, fb, head) : performLegacyTransaction(txn);
    IOLockLock(fTransactionLock);
    if (fTransaction.transaction == txn && fTransaction.sequence == sequence) {
        submitted.key = fTransaction.key; submitted.queued = fTransaction.queued;
    }
    IOLockUnlock(fTransactionLock);
    if (!submitted.queued) finishTransaction(submitted, result);
    if (finalPath && result == kIOReturnSuccess) {
        OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]);
        OSAddAtomic64(1, txn->getPlaneIOSurface(0, 0) ? &gCntIopSrcSurf : &gCntIopSrcRes);
    }
    if (result != kIOReturnSuccess) OSAddAtomic64(1, &gCntIopFail);
    return result;
}

static int nvacceliop_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvIop;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    NVAccel *a = NVAccel::forDisplay(); NVRetain retained(a);
    if (!a) return ENXIO;
    if (v) {
        OSDictionary *caps = OSDictionary::withCapacity(2);
        if (!caps) return ENOMEM;
        caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
        caps->setObject("TransactionsSupported", kOSBooleanTrue);
        a->setProperty("IOAccelDisplayPipeCapabilities", caps);
        caps->release();
        gNvIop = 1;
        ALOG("iop: debug.nvaccel_iop=1 -- capabilities published, transaction hooks OPEN");
    } else {
        gNvIop = 0;
        a->removeProperty("IOAccelDisplayPipeCapabilities");
        ALOG("iop: debug.nvaccel_iop=0 -- capabilities removed, transaction hooks answer as the base class");
    }
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvacceliop_sysctl, "I",
            "K3: 1 = publish IOAccelDisplayPipeCapabilities and perform display-pipe transactions, 0 = behave as the base class (default)");

static int nvaccelfb_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvAccelArmed;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    NVAccel::triggerFromSysctl(v);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccelfb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvaccelfb_sysctl, "I",
            "register the NVIDIA framebuffer with the accelerator display machine (1 = found_framebuffer, 2 = family sweep)");
#define NVACCEL_CNT(nm, var, desc) SYSCTL_QUAD(_debug, OID_AUTO, nm, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&var, desc)
NVACCEL_CNT(nvaccel_vm_alloc,         gCntVmAlloc,        "NVVidMemory::allocPhysical successes");
NVACCEL_CNT(nvaccel_vm_alloc_bytes,   gCntVmAllocBytes,   "bytes of VRAM granted to NVVidMemory");
NVACCEL_CNT(nvaccel_vm_dealloc,       gCntVmDealloc,      "NVVidMemory::deallocPhysical calls with a handle");
NVACCEL_CNT(nvaccel_vm_dealloc_bytes, gCntVmDeallocBytes, "bytes of VRAM given back by NVVidMemory");
NVACCEL_CNT(nvaccel_vm_refused,       gCntVmRefused,      "allocPhysical refused by the framebuffer");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_vm_sticky, CTLFLAG_RW | CTLFLAG_LOCKED, &gVmSticky, 0, "1 = a surface keeps its VRAM across the family's dealloc/alloc (the memory the plugin draws into)");
NVACCEL_CNT(nvaccel_vm_parked,        gCntVmParked,       "VRAM kept when the family freed a surface's VRAM");
NVACCEL_CNT(nvaccel_vm_unparked,      gCntVmUnparked,     "allocations answered with the surface's kept VRAM");
NVACCEL_CNT(nvaccel_vm_park_released, gCntVmParkReleased, "kept VRAM released (resource freed or grown)");
NVACCEL_CNT(nvaccel_res_new,          gCntResNew,         "NVResource objects created");
NVACCEL_CNT(nvaccel_res_free,         gCntResFree,        "NVResource objects freed");
NVACCEL_CNT(nvaccel_map_commit,       gCntMapCommit,      "GPU page-table commits (MAP_MEMORY_DMA ok)");
NVACCEL_CNT(nvaccel_map_release,      gCntMapRelease,     "GPU page-table releases (UNMAP_ONLY)");
NVACCEL_CNT(nvaccel_map_free,         gCntMapFree,        "GPU VA ranges freed (nvAccelGpuVaFree)");
NVACCEL_CNT(nvaccel_iop_ok,           gCntIopOk,          "K3: display-pipe transactions copied to the panel");
NVACCEL_CNT(nvaccel_iop_ok_h0,        gCntIopOkHead[0],   "frames copied to head 0's panel");
NVACCEL_CNT(nvaccel_iop_ok_h1,        gCntIopOkHead[1],   "frames copied to head 1's panel");
NVACCEL_CNT(nvaccel_iop_ok_h2,        gCntIopOkHead[2],   "frames copied to head 2's panel");
NVACCEL_CNT(nvaccel_iop_ok_h3,        gCntIopOkHead[3],   "frames copied to head 3's panel");
NVACCEL_CNT(nvaccel_iop_ok_h4,        gCntIopOkHead[4],   "frames copied to head 4 (an Intel built-in panel, NMIntelFB)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_published, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadsPublished, 0,
            "bit i = head i's framebuffer has published to the accelerator");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_registered, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsRegistered, 0,
            "bit i = head i's framebuffer was handed to the display machine (found_framebuffer)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_piped, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsPiped, 0,
            "bit i = head i has a display pipe that drives ITS OWN framebuffer (pipe+0x98 read back)");
static_assert(kNvMaxFB == 5, "kNvMaxFB changed: add debug.nvaccel_iop_ok_h<i> for every new head");
NVACCEL_CNT(nvaccel_iop_fail,         gCntIopFail,        "K3: transactions refused (see the capped iop: log lines)");
NVACCEL_CNT(nvaccel_iop_empty,        gCntIopEmpty,       "K3: transactions with no plane (gamma/options only)");
NVACCEL_CNT(nvaccel_iop_src_res,      gCntIopSrcRes,      "K3: frames sourced from our VRAM resource");
NVACCEL_CNT(nvaccel_iop_src_surf,     gCntIopSrcSurf,     "K3: frames sourced from the IOSurface memory");
NVACCEL_CNT(nvaccel_iop_30bit,         gCntIop30,          "frames written to a 30-bit scanout");
NVACCEL_CNT(nvaccel_iop_half,          gCntIopHalf,        "frames converted from half-float RGhA");
NVACCEL_CNT(nvaccel_iop_l10r,         gCntIopL10r,        "frames narrowed from 10-bit l10r to the 8-bit scanout");
NVACCEL_CNT(nvaccel_iop_blank,        gCntIopBlank,       "K6: frames copied whose 64 samples were all ONE colour (a blank frame)");
static SInt64 gCntIopCopySkip;
NVACCEL_CNT(nvaccel_iop_copy_skip,    gCntIopCopySkip,    "frames kept on the panel because the surface's newest pixels were not yet in system memory");
static int    gIopFlip = 0;
static SInt64 gCntIopFlip, gCntIopFlipHome, gCntIopFlipNo[8];
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_flip, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gIopFlip, 0, "1 = scan the composited IOSurface out of its own VRAM (no copy)");
NVACCEL_CNT(nvaccel_iop_flips,        gCntIopFlip,        "frames scanned out of the surface's own VRAM (zero copy)");
NVACCEL_CNT(nvaccel_iop_flip_home,    gCntIopFlipHome,    "flips back to the boot scanout before a copied frame");
NVACCEL_CNT(nvaccel_iop_flip_lock,    gCntIopFlipNo[3],   "copied because the accelerator lock was busy");
NVACCEL_CNT(nvaccel_iop_flip_novram,  gCntIopFlipNo[4],   "copied because the surface has no RM VRAM");
NVACCEL_CNT(nvaccel_iop_flip_stale,   gCntIopFlipNo[5],   "copied because system memory holds the newest copy");
NVACCEL_CNT(nvaccel_iop_flip_refused, gCntIopFlipNo[7],   "copied because the framebuffer refused the flip");
static SInt64 gCntIopUnwritten, gCntIopCopyUnwritten;
// hybrid laptops (nm_hybrid.h): boot-arg nmhybrid, default off
SYSCTL_INT(_debug, OID_AUTO, nmhybrid, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNmHybrid, 0,
           "hybrid laptop path: 0 off, 1 completed final-image publication, 2 requested import");
NVACCEL_CNT(nmhybrid_owned,        gCntHyOwned,        "hybrid: frames written into the panel driver's own buffers and flipped to");
NVACCEL_CNT(nmhybrid_dma,          gCntHyDma,          "hybrid: frames the copy engine wrote straight into the panel driver's buffers");
NVACCEL_CNT(nmhybrid_dma_timeouts, gCntHyDmaTimeout,   "hybrid: copy-engine frames that timed out (buffer quarantined)");
NVACCEL_CNT(nmflat_held,            gCntFlatHeld,       "frames of one flat colour held back from the flip (nmflatskip)");
NVACCEL_CNT(nmhybrid_frames,        gCntHyFrames,       "completed final-target to scanout-memory publications; not physical latches");
NVACCEL_CNT(nmhybrid_producer_errors, gCntPresentProducer, "producer completion timeouts");
NVACCEL_CNT(nmhybrid_publication_errors, gCntPresentPublication, "unproved IOSurface publication");
NVACCEL_CNT(nmhybrid_bounds_errors, gCntPresentBounds, "source or target span refusals");
NVACCEL_CNT(nmhybrid_epoch_errors, gCntPresentEpoch, "closed or stale presentation epoch");
NVACCEL_CNT(nmhybrid_copy_errors, gCntPresentCopy, "final target or sink transfer errors");
NVACCEL_CNT(nmhybrid_flips,         gCntHyFlips,        "hybrid: frames scanned out in place (zero copy)");
NVACCEL_CNT(nmhybrid_p2p,           gCntHyP2p,          "hybrid: frames scanned out of the NVIDIA GPU's memory through BAR1 (nmhybridp2p)");
NVACCEL_CNT(nmhybrid_import_fail,   gCntHyImportFail,   "hybrid: surfaces that could not be scanned out in place (copied instead)");
NVACCEL_CNT(nvaccel_iop_flip_unwritten, gCntIopUnwritten,     "copied because the family marked VRAM newest on a copy the plugin never wrote");
NVACCEL_CNT(nvaccel_iop_copy_unwritten, gCntIopCopyUnwritten, "copied frames that skipped the page-off because the plugin never wrote the VRAM copy");
// The two decisions the display pipe makes from the family's validity bits, kept as pure functions so they can be
// exercised outside the kernel (tools/test_iop_flip_gate.py). vramNewest is the family's bit (valid[6] & plane);
// sysmemCurrent is valid[0] & plane; pluginWrote is nvAccelPluginWrote(): the resource's recorded write cookie equals the
// allocation cookie of the VRAM it holds now.
// A zero-copy flip scans VRAM out directly, so it needs a VRAM copy that holds a frame: the family's bit alone is not
// enough, the plugin must have written it (its dirty escape sets both).
static inline bool nvAccelIopVramFlippable(bool vramNewest, bool pluginWrote) { return vramNewest && pluginWrote; }
// A copied frame is read from system memory. When the family says that copy is stale, the resource is paged off VRAM
// first; a VRAM copy the plugin never wrote holds nothing worth paging, and paging it would overwrite the pixels the
// plugin copied out. Such a surface is read as it is.
static inline bool nvAccelPluginWrote(unsigned long long wroteCookie, unsigned long long allocCookie) { return wroteCookie != 0 && wroteCookie == allocCookie; }
static SInt64 gCntIopFlipHit, gCntIopFlipMiss;
NVACCEL_CNT(nvaccel_iop_flip_hit,     gCntIopFlipHit,     "swaps resolved from the cache (no accelerator lock)");
NVACCEL_CNT(nvaccel_iop_flip_miss,    gCntIopFlipMiss,    "swaps that resolved under the accelerator lock");
static int    gIopAsync = 1;

/*
 * Frame completion off WindowServer's thread. NVIDIA's display path never makes the compositor wait for the GPU: a flip
 * completes on the display engine's flip event, and a flip ordered behind a fence waits for it in a worker
 * (nvidia-drm-modeset.c:636/692). Here WindowServer waited for every composite to finish on the GPU before swapping
 * (issue #196: ~1.3 ms of each 6.9 ms frame at 144 Hz, ~95-105 presents/s). Now the plugin marks the surfaces a
 * WindowServer command buffer renders as pending when it commits and ready when the GPU has finished them, and the swap
 * waits for that here: on the async flip thread for a zero-copy flip, before reading for a copied frame. A mark answers
 * 0 while this is on; the plugin keeps WindowServer's own wait when it answers anything else.
 */
static int gSurfGpuOn = 1;
static SInt64 gCntSurfMarks, gCntSurfWaits, gCntSurfTimeouts;
static int gSurfWaitMaxUs;
static const unsigned kSurfGpuWaitMs = 100; // timeout preserves the pending mark and refuses the frame
struct NVSurfPending { unsigned int id; unsigned int seq; };
static NVSurfPending gSurfPend[32];
static unsigned int gSurfPendSeq;
static IOLock *gSurfPendLock;
static IOLock *nvSurfLock()
{
    if (!gSurfPendLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSurfPendLock)) IOLockFree(l); }
    return gSurfPendLock;
}
static bool nvSurfMark(unsigned int id, bool pending)
{
    IOLock *l = nvSurfLock();
    if (!l || !id) return false;
    IOLockLock(l);
    unsigned slot = 32;
    for (unsigned i = 0; i < 32; i++) {
        if (gSurfPend[i].id == id) { slot = i; break; }
        if (slot == 32 && !gSurfPend[i].id) slot = i;
    }
    bool ok = slot != 32;
    if (pending && ok) { gSurfPend[slot].id = id; gSurfPend[slot].seq = ++gSurfPendSeq; }
    else if (!pending && ok && gSurfPend[slot].id == id) {
        gSurfPend[slot].id = 0; IOLockWakeup(l, gSurfPend, false);
    }
    IOLockUnlock(l);
    if (ok) OSAddAtomic64(1, &gCntSurfMarks);
    return ok;
}
static bool nvSurfWaitReady(unsigned int id)
{
    IOLock *l = gSurfPendLock;
    if (!id) return false;
    if (!l) return true;
    uint64_t start = mach_absolute_time(), deadline = 0;
    clock_interval_to_deadline(kSurfGpuWaitMs, kMillisecondScale, &deadline);
    bool waited = false, ready = true;
    IOLockLock(l);
    for (;;) {
        unsigned i = 0;
        while (i < 32 && gSurfPend[i].id != id) i++;
        if (i == 32) break;
        if (mach_absolute_time() >= deadline) { ready = false; break; }
        waited = true;
        IOLockSleepDeadline(l, gSurfPend, deadline, THREAD_UNINT);
    }
    IOLockUnlock(l);
    if (waited) {
        uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
        if ((int)(ns / 1000) > gSurfWaitMaxUs) gSurfWaitMaxUs = (int)(ns / 1000);
        OSAddAtomic64(1, ready ? &gCntSurfWaits : &gCntSurfTimeouts);
    }
    return ready;
}
// True when a GPU write to this surface is still marked pending (no wait).
static bool nvSurfIsPending(unsigned int id)
{
    IOLock *l = gSurfPendLock;
    if (!l || !id) return false;
    bool p = false;
    IOLockLock(l);
    for (unsigned i = 0; i < 32 && !p; i++) p = gSurfPend[i].id == id;
    IOLockUnlock(l);
    return p;
}
// Unlocked page-off wait. A CPU lock of a surface WindowServer's GPU is still writing waits for that write inside
// NVResource::pageoff, which the family calls with the accelerator lock held; the wait (up to 100 ms) held that lock and
// every RM call that needs it (VRAM imports, allocations, dirty marks) queued behind it. With this on, the page-off lets go
// of the lock while it waits, only when the current thread owns it at one level, and takes it back before it goes on.
// Off by default until it has run on a card: debug.nvaccel_pageoff_unlocked_wait=1 turns it on.
static int gPoUnlocked = 0;
static SInt64 gCntPoUnlocked, gCntPoNotOwner;
static int gPoUnlockedMaxUs;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_pageoff_unlocked_wait, CTLFLAG_RW | CTLFLAG_LOCKED, &gPoUnlocked, 0, "1 = a page-off waits for the GPU without the accelerator lock");
NVACCEL_CNT(nvaccel_pageoff_unlocked_waits,   gCntPoUnlocked, "page-offs that waited for the GPU without the accelerator lock");
NVACCEL_CNT(nvaccel_pageoff_locked_not_owner, gCntPoNotOwner, "page-offs with a pending surface that waited locked because the thread was not the lock's owner at one level");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_pageoff_unlocked_wait_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gPoUnlockedMaxUs, 0, "longest unlocked page-off wait, us");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_surface_gpu, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfGpuOn, 0, "1 = WindowServer swaps wait for GPU completion here (same value as nvaccel_surface_gpu_on)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_surface_gpu_on, CTLFLAG_RW | CTLFLAG_LOCKED, &gSurfGpuOn, 0, "0 = WindowServer waits for its own composites again (the plugin re-asks every second)");
NVACCEL_CNT(nvaccel_surface_gpu_marks,    gCntSurfMarks,    "pending/ready marks from WindowServer");
NVACCEL_CNT(nvaccel_surface_gpu_waits,    gCntSurfWaits,    "swaps that waited here for their composite to finish");
NVACCEL_CNT(nvaccel_surface_gpu_timeouts, gCntSurfTimeouts, "swaps whose composite did not finish within 100 ms");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_surface_gpu_wait_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfWaitMaxUs, 0, "longest swap wait for a composite, us");
static SInt64 gCntIopAsync, gCntIopAsyncRefused, gCntIopAsyncDrop;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_async, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopAsync, 0, "1 = the swap runs on a thread call and completes through signalTransactionInterrupt (Apple's shape); 0 = the earlier synchronous flip");
NVACCEL_CNT(nvaccel_iop_async_flips,   gCntIopAsync,        "swaps completed asynchronously");
NVACCEL_CNT(nvaccel_iop_async_refused, gCntIopAsyncRefused, "async swaps NVKMS refused (that frame stayed off the panel)");
NVACCEL_CNT(nvaccel_iop_async_drop,    gCntIopAsyncDrop,    "frames dropped because a flip was still in flight past a refresh period");
static SInt64 gCntIopLate;
NVACCEL_CNT(nvaccel_iop_late,          gCntIopLate,         "frames kept on the panel because the previous flip had not reached it yet (framebuffer answered busy)");
static int gCrcHead = 0;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_crc_head, CTLFLAG_RW | CTLFLAG_LOCKED, &gCrcHead, 0, "b85k-crc: which head debug.nvaccel_crc reads");
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_crc, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, nvaccel_crc_sysctl, "I",
            "b85k-crc: display CRC32 of what head nvaccel_crc_head is showing (0 = unavailable)");
static bool gSysctlInstalled = false;
static void nvaccelInstallSysctl(void)
{
    if (gSysctlInstalled) return;
#ifdef NMX_ACCEL
    // only this one: the NVIDIA accelerator may be loaded too, and owns the debug.nvaccel* names
    sysctl_register_oid(&sysctl__debug_nmxaccel_headless);
    gSysctlInstalled = true;
#else
    sysctl_register_oid(&sysctl__debug_nvaccelfb);
    sysctl_register_oid(&sysctl__debug_nvaccel_headless);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc);       sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc);     sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_sticky);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_parked);      sysctl_register_oid(&sysctl__debug_nvaccel_vm_unparked);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_park_released);
    sysctl_register_oid(&sysctl__debug_nvaccel_res_new);        sysctl_register_oid(&sysctl__debug_nvaccel_res_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_commit);     sysctl_register_oid(&sysctl__debug_nvaccel_map_release);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok);         sysctl_register_oid(&sysctl__debug_nvaccel_iop_fail);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_empty);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_res);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_surf); sysctl_register_oid(&sysctl__debug_nvaccel_iop_l10r); sysctl_register_oid(&sysctl__debug_nvaccel_iop_30bit); sysctl_register_oid(&sysctl__debug_nvaccel_iop_half);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_blank);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_skip);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip);       sysctl_register_oid(&sysctl__debug_nvaccel_iop_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_home);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_lock);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_novram); sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_stale);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_unwritten); sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_unwritten);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_crc_head);       sysctl_register_oid(&sysctl__debug_nvaccel_crc);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_hit);   sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_miss);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_flips);
    { extern struct sysctl_oid sysctl__debug_nvaccel_svdiag; sysctl_register_oid(&sysctl__debug_nvaccel_svdiag); }
    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu);    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_on);
    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_marks); sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_waits);
    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_timeouts); sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_wait_max_us);
    sysctl_register_oid(&sysctl__debug_nvaccel_pageoff_unlocked_wait); sysctl_register_oid(&sysctl__debug_nvaccel_pageoff_unlocked_waits);
    sysctl_register_oid(&sysctl__debug_nvaccel_pageoff_locked_not_owner); sysctl_register_oid(&sysctl__debug_nvaccel_pageoff_unlocked_wait_max_us);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_refused); sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_drop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_late);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h0);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h1);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h2);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h3);
    { int lv = 0; const int present = PE_parse_boot_argn("nmhybrid", &lv, sizeof lv);
      gNmHybrid = nmh_level_parse(present, lv);
      if (present) ALOG("hybrid: switch level %d (boot-arg nmhybrid=%d)%s", gNmHybrid, lv, gNmHybrid ? "" : " -- not 1 or 2, so OFF");
      int noIgpu = 0;
      if (nm_optc_should_enable_igpu(gNmHybrid, PE_parse_boot_argn("-nvaccelnoigpu", &noIgpu, sizeof noIgpu))) {
          gNvIgpu = 1;
          ALOG("hybrid: Option C auto-enabled debug.nvaccel_igpu=1 (nmhybrid=%d)", gNmHybrid);
      } }
    { int pv = 0; const int present = PE_parse_boot_argn("nmhybridp2p", &pv, sizeof pv);
      gNmHybridP2P = nmp_parse(present, pv);
      if (present) ALOG("hybrid: zero-copy from VRAM mode %d (boot-arg nmhybridp2p=%d)", gNmHybridP2P, pv); }
    { int fv = 0; const int present = PE_parse_boot_argn("nmflatskip", &fv, sizeof fv);
      gNmFlatSkip = nmh_flat_parse(present, fv);
      if (gNmFlatSkip) ALOG("flat frames: up to %u in a row are held back (boot-arg nmflatskip)", gNmFlatSkip); }
    sysctl_register_oid(&sysctl__debug_nmflat_held);
    sysctl_register_oid(&sysctl__debug_nmhybrid_p2p); sysctl_register_oid(&sysctl__debug_nmhybrid_owned); sysctl_register_oid(&sysctl__debug_nmhybrid_dma); sysctl_register_oid(&sysctl__debug_nmhybrid_dma_timeouts);
    sysctl_register_oid(&sysctl__debug_nmhybrid);
    sysctl_register_oid(&sysctl__debug_nmhybrid_frames);
    sysctl_register_oid(&sysctl__debug_nmhybrid_producer_errors); sysctl_register_oid(&sysctl__debug_nmhybrid_publication_errors);
    sysctl_register_oid(&sysctl__debug_nmhybrid_bounds_errors); sysctl_register_oid(&sysctl__debug_nmhybrid_epoch_errors);
    sysctl_register_oid(&sysctl__debug_nmhybrid_copy_errors);
    sysctl_register_oid(&sysctl__debug_nmhybrid_flips);
    sysctl_register_oid(&sysctl__debug_nmhybrid_import_fail);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h4);      sysctl_register_oid(&sysctl__debug_nvaccel_igpu);
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_published); sysctl_register_oid(&sysctl__debug_nvaccel_heads_registered);
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_piped);
    gSysctlInstalled = true;
    ALOG("sysctl debug.nvaccelfb installed -- `sudo sysctl -w debug.nvaccelfb=1` registers the framebuffer");
#endif
}


IOService *NVAccel::locateNVRM()
{
    if (fNVRM) return fNVRM;
    if (!fPCI || fStopping) return nullptr;
    OSIterator *it = fPCI->getClientIterator();
    if (it) {
        while (OSObject *object = it->getNextObject()) {
            IOService *s = OSDynamicCast(IOService, object);
            if (s && s->metaCast("NVRM") && s->getProvider() == fPCI && !s->isInactive()) {
                s->retain(); fNVRM = s; break;
            }
        }
        it->release();
    }
    return fNVRM;
}
static IOService *nvAccelNVRM(NVAccel *owner)
{ return owner && owner->fOwner ? owner->fOwner->fNvrm : nullptr; }


#define NVRM_SS_KERNEL_ABI 1
#include "../nvrm_surfshare_abi.h"
extern "C" {
void *_ZN13IOSurfaceRoot13lookupSurfaceEjP4task(void *root, unsigned int id, task_t task);
void *_ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej(void *accel, void *surface, unsigned int plane);
void  _ZN16IOAccelVidMemory4wireEv(void *vidmem);
void  _ZN20IOSurfaceDeviceCache10setCurrentEv(void *cache);
#if defined(NM_ERA) && NM_ERA < 1015
}
static inline void *_ZNK9IOSurface19getMemoryDescriptorEv(void *surface) { return _ZN9IOSurface19getMemoryDescriptorEv(surface); }
static inline unsigned int _ZNK9IOSurface12getSurfaceIDEv(void *surface) { return ((const IOSurface *)surface)->getSurfaceID(); }
extern "C" {
#else
void *_ZNK9IOSurface19getMemoryDescriptorEv(void *surface);
unsigned int _ZNK9IOSurface12getSurfaceIDEv(void *surface);
#endif
void  _ZN22IOGraphicsAccelerator29lock_busyEv(void *accel);
void  _ZN22IOGraphicsAccelerator211unlock_busyEv(void *accel);
}
static bool nvFamLock(NVAccel *a, bool tryOnly)
{
    unsigned char *b = (unsigned char *)a;
    IOLock *lk = *(IOLock *volatile *)(b + 0x88);
    if (!lk) return false;
    if (tryOnly) { if (!IOLockTryLock(lk)) return false; }
    else {
#if defined(NM_ERA) && NM_ERA < 1100
        // The waiter count beside the lock is not confirmed on these releases; the lock itself is (same place on all).
        IOLockLock(lk);
#else
        OSIncrementAtomic((volatile SInt32 *)(b + 0x90));
        IOLockLock(lk);
        OSDecrementAtomic((volatile SInt32 *)(b + 0x90));
#endif
    }
    _ZN22IOGraphicsAccelerator29lock_busyEv(a);
    a->acceleratorDidLock("", 0);
    return true;
}
static void nvFamUnlock(NVAccel *a)
{
    a->acceleratorWillUnlock("", 0);
    _ZN22IOGraphicsAccelerator211unlock_busyEv(a);
    IOLockUnlock(*(IOLock *volatile *)((unsigned char *)a + 0x88));
}
enum { kNVSVPend = 512 };
static unsigned gNVSVPendId[kNVSVPend];
static NVResource *gNVSVPendResource[kNVSVPend];
static NVBackingKey gNVSVPendKey[kNVSVPend];
static IOSimpleLock *gNVSVPendLock;
static bool nvSVPend(unsigned id, int op, NVResource *resource = nullptr)
{
    if (!id || !gNVSVPendLock) return false;
    NVVidMemory *memory = resource ? nvres_vidmem(resource) : nullptr;
    const NVBackingKey key = memory && memory->fBacking ? memory->fBacking->key() : NVBackingKey{};
    if (op == 1 && (!resource || !key.allocationGeneration)) return false;
    bool r = false; int freeSlot = -1;
    IOSimpleLockLock(gNVSVPendLock);
    for (int i = 0; i < kNVSVPend; i++) {
        if (gNVSVPendId[i] == id) {
            r = true;
            if (op == 0 && resource == gNVSVPendResource[i] && nvBackingKeyEqual(key, gNVSVPendKey[i])) {
                gNVSVPendId[i] = 0; gNVSVPendResource[i] = nullptr; gNVSVPendKey[i] = {};
            } else if (op == 1) { gNVSVPendResource[i] = resource; gNVSVPendKey[i] = key; }
            break;
        }
        if (!gNVSVPendId[i] && freeSlot < 0) freeSlot = i;
    }
    if (op == 1 && !r && freeSlot >= 0) {
        gNVSVPendId[freeSlot] = id; gNVSVPendResource[freeSlot] = resource; gNVSVPendKey[freeSlot] = key; r = true;
    }
    IOSimpleLockUnlock(gNVSVPendLock);
    return r;
}
// A pending mark belongs to the resource that last dirtied the surface. Never
// dereference a device cache during destruction, or clear a newer owner's mark.
static void nvSVForgetResource(NVResource *resource)
{
    if (!resource || !gNVSVPendLock) return;
    IOSimpleLockLock(gNVSVPendLock);
    for (unsigned i = 0; i < kNVSVPend; i++) if (gNVSVPendResource[i] == resource) {
        gNVSVPendId[i] = 0;
        gNVSVPendResource[i] = nullptr;
    }
    IOSimpleLockUnlock(gNVSVPendLock);
}
static void    *gNVSVRoot;
static unsigned gNVSVOk, gNVSVRefused;
static inline void nvSVZero(void *p, unsigned long n) { volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static unsigned gNVSVDirty, gNVSVDirtyRefused, gNVSVPageoff, gNVSVPageoffFail;
// sysctl debug.nvaccel_svdiag=N logs the next N surface dirty marks and page-offs (surface, result, path); 0 = off
static int gSVDiag;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_svdiag, CTLFLAG_RW | CTLFLAG_LOCKED, &gSVDiag, 0, "log the next N surface dirty marks and page-offs");
static bool nvSVDiag() { return gSVDiag > 0 && OSDecrementAtomic(&gSVDiag) > 0; }
static unsigned nvAccelSurfDirty(void *cache, NVResource *res, unsigned plane)
{
    if (!res || !res->fAccel || res->fAccel->fCandidate) return NVRM_SS_OFF;
    volatile unsigned char *r = (volatile unsigned char *)res;
    unsigned char type = r[kNvResType];
    unsigned int fl = *(volatile unsigned int *)(r + kNvResFlags);
    volatile unsigned short *valid = *(unsigned short *volatile *)(r + kNvResValid);
    if (type != 0xc0 || (fl & 0x8000) || !valid || plane > 15) return NVRM_SS_NO_CACHE;
    if (!gNVSVPendLock) {
        IOSimpleLock *l = IOSimpleLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNVSVPendLock)) IOSimpleLockFree(l);
    }
    if (!nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(*(void *volatile *)((unsigned char *)cache + 0x10)), 1, res)) return NVRM_SS_NO_CACHE;
    _ZN20IOSurfaceDeviceCache10setCurrentEv(cache);
    valid[6] |= (unsigned short)(1u << plane);
    valid[0] &= (unsigned short)~(1u << plane);
    { NVVidMemory *vm = nvres_vidmem(res); res->fPluginWroteCookie = vm ? vm->cookie() : 0; }
    return 0;
}
// The caller holds the accelerator family lock. The plugin can still write a
// retained allocation after the family unwires it; make that same backing active
// before marking the GPU copy current. Never allocate fresh memory for this case.
static bool nvAccelRestoreParkedForDirty(NVResource *res, NVVidMemory *vm, unsigned plane)
{
    if (!res || !vm || !vm->fOwner || vm->fOwner->fDirect) return false;
    if (vm->fBacking) return true;
    if (!vm->fParked || plane > 15 || static_cast<IOAccelMemory *>(vm)->getLength() > vm->fParked->fReceipt.actualSize) return false;
    const NVBackingKey key = vm->fParked->key();
    _ZN16IOAccelVidMemory4wireEv(vm);
    return vm->fBacking && !vm->fParked && nvBackingKeyEqual(key, vm->fBacking->key());
}
// The marks arrive through NVK's surface escape (NVRM_ESC_SURF_DIRTY with plane NVRM_SS_PLANE_GPU_MARK | pending): the
// WindowServer sandbox denies sysctl writes. Only WindowServer's own marks are taken, so no other process can hold a swap.
static unsigned nvSurfGpuEscape(const struct NVRMSurfShareEsc *p)
{
    if (!gSurfGpuOn) return NVRM_SS_OFF;
    char nm[32] = { 0 };
    proc_selfname(nm, sizeof nm);
    if (strcmp(nm, "WindowServer")) return NVRM_SS_OFF;
    if (p->surfaceID && !nvSurfMark(p->surfaceID, (p->plane & 1u) != 0)) return NVRM_SS_BUSY;
    return 0;
}
static IOReturn nvAccelSurfVram(NVAccel *owner, struct NVRMSurfShareEsc *p, task_t task, unsigned cmd)
{
    if (!p) return kIOReturnBadArgument;
    p->hSrcClient = p->hSrcMemory = 0;
    if (p->version != NVRM_SURFSHARE_VERSION) { p->status = NVRM_SS_BAD_VERSION; return kIOReturnSuccess; }
    // Refusal includes capability probes. No lookup, wire, share or dirty transition may precede it.
    if (!owner || owner->fCandidate || owner->fStopping) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    if (cmd == NVRM_ESC_SURF_DIRTY && (p->plane & NVRM_SS_PLANE_GPU_MARK)) {
        p->status = nvSurfGpuEscape(p); return kIOReturnSuccess;
    }
    // The plugin re-marks a surface after its last GPU write on the thread that releases the pending marks. A page-off
    // waiting for one of those marks holds the accelerator lock, so that re-mark must not wait for the lock: it is
    // dropped instead, and the mark taken at commit stands.
    const bool tryOnly = cmd == NVRM_ESC_SURF_DIRTY && (p->plane & NVRM_SS_PLANE_TRYLOCK);
    p->plane &= ~(NVRM_SS_PLANE_TRYLOCK | NVRM_SS_PLANE_GPU_MARK);
    // plane is a full uint32 from the caller and reaches IOGraphicsAccelerator2::deviceCacheForIOSurface
    // below. The plane > 15 refusals further down run after that call, and only on the dirty path.
    if (p->plane > 15) { p->status = NVRM_SS_NO_CACHE; return kIOReturnSuccess; }
    NVAccel *a = NVAccel::resolve(owner);
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    if (!task || !a || !a->fOwner || !a->fOwner->handles(&h)) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    if (!gNVSVRoot) {
        OSDictionary *m = IOService::serviceMatching("IOSurfaceRoot");
        if (m) { gNVSVRoot = IOService::waitForMatchingService(m, 5ULL * 1000 * 1000 * 1000); m->release(); }
        if (!gNVSVRoot) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    }
    void *surf = _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(gNVSVRoot, p->surfaceID, task);
    if (!surf) { p->status = NVRM_SS_NO_SURFACE; gNVSVRefused++; return kIOReturnSuccess; }
    if (!nvFamLock(a, tryOnly)) { ((OSObject *)surf)->release(); p->status = tryOnly ? NVRM_SS_BUSY : NVRM_SS_OFF; return kIOReturnSuccess; }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, p->plane);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    if (cmd == NVRM_ESC_SURF_DIRTY) {
        const bool sameBacking = nvAccelRestoreParkedForDirty(res, vm, p->plane);
        p->status = (sameBacking && cache && res && vm && vm->handle())
            ? nvAccelSurfDirty(cache, res, p->plane) : NVRM_SS_NO_CACHE;
        if (p->status == 0) gNVSVDirty++; else gNVSVDirtyRefused++;
        if (nvSVDiag()) ALOG("SVDIAG dirty surface %u plane %u -> %#x (cache %d res %d vm %d)", p->surfaceID, p->plane, p->status,
                             cache ? 1 : 0, res ? 1 : 0, (vm && vm->handle()) ? 1 : 0);
        if (gNVSVDirty + gNVSVDirtyRefused <= 6 || ((gNVSVDirty + gNVSVDirtyRefused) & 4095) == 0)
            ALOG("surfvram dirty: surface %u plane %u type 0x%x -> %#x (dirty %u, refused %u)", p->surfaceID, p->plane,
                 res ? (unsigned)((volatile unsigned char *)res)[kNvResType] : 0u, p->status, gNVSVDirty, gNVSVDirtyRefused);
        if (cache) ((OSObject *)cache)->release();
        nvFamUnlock(a);
        ((OSObject *)surf)->release();
        return kIOReturnSuccess;
    }
    if (vm && !vm->handle() && !vm->fScanout) _ZN16IOAccelVidMemory4wireEv(vm);
    if (!cache || !res) p->status = NVRM_SS_NO_CACHE;
    else if (!vm || !vm->handle() || !vm->capacity()) p->status = NVRM_SS_NO_VRAM;
    else if (p->size > vm->capacity()) p->status = NVRM_SS_TOO_SMALL;
    else {
        struct NVRMSurfShareRm g; nvSVZero(&g, sizeof g);
        g.version = NVRM_SS_RM_VERSION; g.op = NVRM_SS_RM_SHARE_KAPI;
        g.kapiDevice = h.kapiDevice; g.kapiMemory = vm->handle(); g.target = p->hClient; g.status = NVRM_SS_OFF;
        IOService *nvrm = nvAccelNVRM(a);
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_RM);
        if (nvrm && sym && nvrm->callPlatformFunction(sym, false, &g, nullptr, nullptr, nullptr) == kIOReturnSuccess) {}
        if (sym) sym->release();
        p->status = g.status; p->hSrcClient = g.hClient; p->hSrcMemory = g.hObject; p->size = vm->capacity();
    }
    if (p->status == 0) gNVSVOk++; else gNVSVRefused++;
    if (gNVSVOk + gNVSVRefused <= 8 || ((gNVSVOk + gNVSVRefused) & 1023) == 0)
        ALOG("surfvram: surface %u plane %u -> cache %p res %p vidmem %p (%llu B) -> client %#x mem %#x status %#x (ok %u, refused %u)",
             p->surfaceID, p->plane, cache, res, vm, vm ? vm->capacity() : 0ull, p->hSrcClient, p->hSrcMemory, p->status, gNVSVOk, gNVSVRefused);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
    ((OSObject *)surf)->release();
    return kIOReturnSuccess;
}

#include <mach/mach_time.h>
static IOLock *volatile gNVSVStageLock;
static IOBufferMemoryDescriptor *gNVSVStageBuf;
static unsigned gNVSVStaged, gNVSVStageNoBuf, gNVSVStageBackoff;
static IOLock *nvSVStageLock(void)
{
    if (!gNVSVStageLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNVSVStageLock)) IOLockFree(l);
    }
    return gNVSVStageLock;
}
static IOBufferMemoryDescriptor *nvSVStage(unsigned long long len)
{
    if (gNVSVStageBuf && gNVSVStageBuf->getLength() >= len) return gNVSVStageBuf;
    if (len > (64ull << 20) || gNVSVStageBackoff) { if (gNVSVStageBackoff) gNVSVStageBackoff--; return nullptr; }
    const unsigned long long want = (len + 0xFFFFFull) & ~0xFFFFFull;
    IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                                      (vm_size_t)want, PAGE_SIZE);
    if (b && b->prepare() != kIOReturnSuccess) { b->release(); b = nullptr; }
    if (!b) {
        gNVSVStageNoBuf++; gNVSVStageBackoff = 1024;
        ALOG("surfvram pageoff: no %llu B physically contiguous staging buffer (miss %u) -- per-page copies for the next 1024", want, gNVSVStageNoBuf);
        return nullptr;
    }
    if (gNVSVStageBuf) { gNVSVStageBuf->complete(); gNVSVStageBuf->release(); }
    gNVSVStageBuf = b;
    ALOG("surfvram pageoff: staging buffer %llu B, physically contiguous", want);
    return b;
}

// The family's own page-off. From 10.12 on it forwards to the short form, whose base body is empty; before 10.12 the
// family has no body at all, so there is nothing to call.
#if defined(NM_ERA) && NM_ERA < 1012
#define NV_FAM_PAGEOFF(ev, a1, out, a3) ((void)(ev), (void)(a1), (void)(out), (void)(a3))
#else
#define NV_FAM_PAGEOFF(ev, a1, out, a3) IOAccelResource2::pageoff(ev, a1, out, a3)
#endif
void NVResource::pageoff(IOAccelEvent *ev, bool a1, bool *out, unsigned long long a3)
{
    if (out) *out = false;
    if (fAccel && fAccel->fCandidate) {
        // Private Vulkan images publish into IOSurface system memory before completion.
        // An unexpected GPU-only dirty resource is a contract violation, never a BAR1 readback.
        const bool needsReadback = requirePageoff();
        if (needsReadback) {
            if (++gNVSVPageoffFail <= 8) ALOG("private publication: GPU-only pageoff refused");
        } else if (out) *out = true;
        return;
    }
    pageoffLegacy(ev, a1, out, a3);
}
void NVResource::pageoffLegacy(IOAccelEvent *ev, bool a1, bool *out, unsigned long long a3)
{
    volatile unsigned char *r = (volatile unsigned char *)this;
    void *cache = *(void *volatile *)(r + kNvResCache);
    NVVidMemory *vm = nvres_vidmem(this);
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    void *surf = (r[kNvResType] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
    if (nvSVDiag()) {
        volatile unsigned short *vd = *(unsigned short *volatile *)(r + kNvResValid);
        ALOG("SVDIAG pageoff surface %u type 0x%x vm %d rm %d scanout %d valid0 %#x valid6 %#x", surf ? _ZNK9IOSurface12getSurfaceIDEv(surf) : 0u,
             (unsigned)r[kNvResType], vm ? 1 : 0, (vm && vm->handle()) ? 1 : 0, vm ? (int)vm->fScanout : -1, vd ? vd[0] : 0xffff, vd ? vd[6] : 0xffff);
    }
    NVVramLease allocation;
    if (!surf || !vm || !vm->fBacking || !vm->fBacking->acquire(NVLeaseUse::CpuAccess, &allocation) ||
        !vm->fOwner->handles(&h)) {
        NV_FAM_PAGEOFF(ev, a1, out, a3);
        return;
    }
    // a CPU lock that arrives while WindowServer's GPU is still writing this surface waits for that write first, as a
    // normal IOSurfaceLock does on Apple's drivers (the commit marked it pending; see nvmtl_ws_mark_pending)
    // While the accelerator lock is free (the unlocked wait below) another thread may drop the last reference to this
    // resource or to its accelerator. Both are held until this function returns, so nothing here ever reads freed memory;
    // whatever the family changed meanwhile is read again after the lock is back.
    struct NVHold { OSObject *o = nullptr; ~NVHold() { if (o) o->release(); } } holdRes, holdAccel;
    {   const unsigned sid = _ZNK9IOSurface12getSurfaceIDEv(surf);
        NVAccel *a = NVAccel::resolve(fAccel);
        if (gPoUnlocked && a && nvSurfIsPending(sid) && a->mayRelease()) {
            const uint64_t t0 = mach_absolute_time();
            retain(); holdRes.o = this;
            a->retain(); holdAccel.o = a;
            nvFamUnlock(a);
            const bool ready = nvSurfWaitReady(sid);
            const bool locked = nvFamLock(a, false);
            if (!ready || !locked) return;
            uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
            if ((int)(ns / 1000) > gPoUnlockedMaxUs) gPoUnlockedMaxUs = (int)(ns / 1000);
            OSAddAtomic64(1, &gCntPoUnlocked);
            // while the lock was free the family may have changed the resource: read everything again
            cache = *(void *volatile *)(r + kNvResCache);
            vm = nvres_vidmem(this);
            surf = (r[kNvResType] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
            if (!surf || !vm || !vm->fBacking || !nvBackingKeyEqual(vm->fBacking->key(), allocation.get()->key()) ||
                !vm->fOwner->handles(&h)) return;
        } else {
            if (gPoUnlocked && a && nvSurfIsPending(sid)) OSAddAtomic64(1, &gCntPoNotOwner);
            if (!nvSurfWaitReady(sid)) return;
        }
    }
    IOMemoryDescriptor *md = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
    if (!md || md->prepare(kIODirectionNone) != kIOReturnSuccess) {
        gNVSVPageoffFail++;
        ALOG("surfvram pageoff: res %p surface pages could not be wired (md %p) -- family pageoff instead", this, md);
        NV_FAM_PAGEOFF(ev, a1, out, a3);
        return;
    }
    bool mayUnwire = true;
    unsigned long long len = md->getLength();
    if (!len || len > vm->capacity() || len > UINT64_MAX - 4095) { md->complete(kIODirectionNone); return; }
    unsigned long long pages = (len + 4095) >> 12;
    bool ok = false; unsigned st = NVRM_SS_OFF; unsigned long long usec = 0, cpuUs = 0;
    bool staged = false;
    if (IOLock *lk = nvSVStageLock()) {
        IOLockLock(lk);
        IOBufferMemoryDescriptor *sb = nvSVStage(len);
        IOByteCount sl = 0;
        addr64_t sp = sb ? sb->getPhysicalSegment(0, &sl, kIOMemoryMapperNone) : 0;
        unsigned long long *spa = (sp && sl >= len && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *spa) : nullptr;
        if (spa) {
            for (unsigned long long k = 0; k < pages; k++) spa[k] = sp + k * 4096;
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->handle();
            q.pages = spa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM(fAccel);
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec;
            if (st == 0) {
                const uint64_t t0 = mach_absolute_time();
                const IOByteCount w = md->writeBytes(0, sb->getBytesNoCopy(), (IOByteCount)len);
                cpuUs = (mach_absolute_time() - t0) / 1000;
                ok = (w == (IOByteCount)len);
                if (!ok) st = NVRM_SS_TOO_SMALL;
                else gNVSVStaged++;
            } else if (st == 0x65u) {
                ALOG("surfvram pageoff: CE copy TIMED OUT into the staging buffer -- abandoning it (%llu B)", (unsigned long long)sb->getLength());
                gNVSVStageBuf = nullptr;
            }
            staged = true;
            IOFree(spa, pages * sizeof *spa);
        }
        IOLockUnlock(lk);
    }
    unsigned long long *pa = (!staged && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *pa) : nullptr;
    if (pa) {
        unsigned long long i = 0;
        for (IOByteCount off = 0; i < pages; ) {
            IOByteCount seglen = 0;
            addr64_t phys = md->getPhysicalSegment(off, &seglen, kIOMemoryMapperNone);
            if (!phys || !seglen) break;
            for (IOByteCount k = 0; k < seglen && i < pages; k += 4096, off += 4096) pa[i++] = phys + k;
        }
        if (i == pages) {
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->handle();
            q.pages = pa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM(fAccel);
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec; ok = st == 0;
            if (st == 0x65u) mayUnwire = false;
        }
        IOFree(pa, pages * sizeof *pa);
    }
    if (mayUnwire) md->complete(kIODirectionNone);
    else ALOG("surfvram pageoff: res %p CE copy TIMED OUT -- surface pages stay wired (%llu B)", this, len);
    if (ok) {
        nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(surf), 0, this);
        volatile unsigned short *valid = *(unsigned short *volatile *)(r + kNvResValid);
        if (valid) valid[0] |= 1;
        gNVSVPageoff++;
    } else gNVSVPageoffFail++;
    if (out) *out = ok;
    if (gNVSVPageoff + gNVSVPageoffFail <= 8 || ((gNVSVPageoff + gNVSVPageoffFail) & 1023) == 0)
        ALOG("surfvram pageoff: res %p %llu B on the copy engine -> %#x in %llu us%s + cpu %llu us (ok %u, failed %u, staged %u)",
             this, len, st, usec, staged ? " (staged)" : " (per page)", cpuUs, gNVSVPageoff, gNVSVPageoffFail, gNVSVStaged);
}

static IOSurface *gIopFlipFront[kNvMaxFB], *gIopFlipPrev[kNvMaxFB];
static bool gIopFlipped[kNvMaxFB];
static bool nvAccelIopFlipNo(int why) { OSAddAtomic64(1, &gCntIopFlipNo[why & 7]); return false; }
struct NVIopFlipCache { IOSurface *s; NVResource *res; NVVramAllocation *allocation; uint64_t use; };
static NVIopFlipCache gIopFlipCache[8];
static IOSimpleLock *gIopFlipCacheLock;
static uint64_t gIopFlipCacheTick;
struct NVFlipView {
    NVResource *res = nullptr;
    NVVramAllocation *allocation = nullptr;
    ~NVFlipView() { if (res) res->release(); if (allocation) allocation->release(); }
};
static bool nvAccelIopCacheGet(IOSurface *surface, NVFlipView *out)
{
    if (!gIopFlipCacheLock) return false;
    IOSimpleLockLock(gIopFlipCacheLock);
    for (unsigned i = 0; i < 8; ++i) if (gIopFlipCache[i].s == surface) {
        out->res = gIopFlipCache[i].res; out->allocation = gIopFlipCache[i].allocation;
        out->res->retain(); out->allocation->retain();
        gIopFlipCache[i].use = ++gIopFlipCacheTick;
        break;
    }
    IOSimpleLockUnlock(gIopFlipCacheLock);
    return out->allocation != nullptr;
}
static void nvAccelIopCachePut(IOSurface *surface, NVResource *res, NVVramAllocation *allocation)
{
    if (!gIopFlipCacheLock) {
        IOSimpleLock *l = IOSimpleLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gIopFlipCacheLock)) IOSimpleLockFree(l);
    }
    if (!gIopFlipCacheLock) return;
    ((OSObject *)surface)->retain(); res->retain(); allocation->retain();
    IOSimpleLockLock(gIopFlipCacheLock);
    unsigned k = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (!gIopFlipCache[i].s || gIopFlipCache[i].s == surface) { k = i; break; }
        if (gIopFlipCache[i].use < gIopFlipCache[k].use) k = i;
    }
    NVIopFlipCache old = gIopFlipCache[k];
    gIopFlipCache[k] = { surface, res, allocation, ++gIopFlipCacheTick };
    IOSimpleLockUnlock(gIopFlipCacheLock);
    if (old.s) ((OSObject *)old.s)->release();
    if (old.res) old.res->release();
    if (old.allocation) old.allocation->release();
}
static NVVramAllocation *gIopFrontBacking[kNvMaxFB], *gIopPrevBacking[kNvMaxFB];
static IOLock *gIopAsLock;
static bool nvIopLockReady()
{
    if (!gIopAsLock) {
        IOLock *lock = IOLockAlloc();
        if (lock && !OSCompareAndSwapPtr(nullptr, lock, (void *volatile *)&gIopAsLock)) IOLockFree(lock);
    }
    return gIopAsLock != nullptr;
}
static void nvDropPresentLease(NVVramAllocation *a)
{ if (a) { a->releaseLease(NVLeaseUse::Present); a->release(); } }
static void nvIopAdoptFront(unsigned head, IOSurface *surface, NVVramAllocation *allocation)
{
    IOLockLock(gIopAsLock);
    IOSurface *old = gIopFlipPrev[head]; NVVramAllocation *previous = gIopPrevBacking[head];
    gIopFlipPrev[head] = gIopFlipFront[head]; gIopPrevBacking[head] = gIopFrontBacking[head];
    gIopFlipFront[head] = surface; gIopFrontBacking[head] = allocation; gIopFlipped[head] = true;
    IOLockUnlock(gIopAsLock);
    if (old) ((OSObject *)old)->release();
    nvDropPresentLease(previous);
}
extern "C" {
#if defined(NM_ERA) && NM_ERA < 1015
// one display-pipe class on these releases, so the same function carries that class's name
void _ZN19IOAccelDisplayPipe226signalTransactionInterruptEPv(void *pipe, void *arg);
#define _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv _ZN19IOAccelDisplayPipe226signalTransactionInterruptEPv
#else
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
#endif
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter(struct thread_call *call);
}
struct NVIopAsyncFrame {
    IOSurface *surface;
    NVVramAllocation *allocation; // one acquired Present lease, not just an OSObject reference
    NVRMFlipRequest request;
    NVTransactionState transaction;
};
struct NVIopAsync {
    NVDisplayPipe *pipe; IOService *fb; NVVramOwner *owner;
    NVIopAsyncFrame current, next;
    volatile SInt32 busy;
    struct thread_call *call;
    bool stopping;
};
static NVIopAsync gIopAs[kNvMaxFB];
static void nvIopDropFrame(NVIopAsyncFrame *frame)
{
    if (frame->surface) ((OSObject *)frame->surface)->release();
    nvDropPresentLease(frame->allocation);
    if (frame->transaction.transaction) ((OSObject *)frame->transaction.transaction)->release();
    *frame = {};
}
static void nvAccelIopAsyncRun(void *p0, void *)
{
    NVIopAsync *e = (NVIopAsync *)p0;
    const unsigned head = (unsigned)(e - gIopAs);
    for (;;) {
        IOLockLock(gIopAsLock);
        NVIopAsyncFrame frame = e->current; e->current = {};
        bool stopping = e->stopping;
        IOLockUnlock(gIopAsLock);
        IOReturn result = kIOReturnNotReady;
        if (frame.surface && !stopping && e->owner->active()) {
            if (!nvSurfWaitReady(frame.surface->getSurfaceID())) result = kIOReturnTimeout;
            else {
                const OSSymbol *s = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
                result = s ? e->fb->callPlatformFunction(s, false, &frame.request, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
                if (s) s->release();
            }
        }
        if (result == kIOReturnSuccess) {
            nvIopAdoptFront(head, frame.surface, frame.allocation);
            frame.surface = nullptr; frame.allocation = nullptr;
            OSAddAtomic64(1, &gCntIopFlip); OSAddAtomic64(1, &gCntIopAsync);
            OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]);
        } else OSAddAtomic64(1, &gCntIopAsyncRefused);
        e->pipe->finishTransaction(frame.transaction, result);
        nvIopDropFrame(&frame);
        _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(e->pipe, nullptr);
        IOLockLock(gIopAsLock);
        if (!e->stopping && e->next.surface) {
            e->current = e->next; e->next = {};
            IOLockUnlock(gIopAsLock);
        } else {
            e->busy = 0;
            IOLockWakeup(gIopAsLock, (event_t)&e->busy, false);
            IOLockUnlock(gIopAsLock);
            break;
        }
    }
}
static bool nvAccelIopAsyncStart(NVDisplayPipe *pipe, IOService *fb, unsigned head, IOSurface *surface,
                                const NVRMFlipRequest *request, NVVramLease *lease)
{
    if (!nvIopLockReady() || !pipe->fAccel || pipe->fAccel->fStopping || pipe->fFramebufferPhase != NVFramebufferPhase::Ready) return false;
    NVIopAsync *e = &gIopAs[head];
    NVIopAsyncFrame displaced = {};
    IOLockLock(gIopAsLock);
    if (e->stopping || (e->pipe && (e->pipe != pipe || e->fb != fb))) { IOLockUnlock(gIopAsLock); return false; }
    if (!e->call) {
        e->call = thread_call_allocate(nvAccelIopAsyncRun, e);
        if (!e->call || !pipe->fAccel->fOwner->borrow()) {
            if (e->call) { thread_call_free(e->call); e->call = nullptr; }
            IOLockUnlock(gIopAsLock); return false;
        }
        e->owner = pipe->fAccel->fOwner;
        e->pipe = pipe; pipe->retain(); e->fb = fb; fb->retain();
    }
    NVIopAsyncFrame frame = {};
    frame.surface = surface; ((OSObject *)surface)->retain();
    frame.allocation = lease->take(); frame.request = *request;
    IOLockLock(pipe->fTransactionLock); pipe->fTransaction.queued = true; frame.transaction = pipe->fTransaction; IOLockUnlock(pipe->fTransactionLock);
    frame.transaction.key = frame.allocation->key();
    if (frame.transaction.transaction) ((OSObject *)frame.transaction.transaction)->retain();
    if (e->busy) { displaced = e->next; e->next = frame; }
    else { e->current = frame; e->busy = 1; thread_call_enter(e->call); }
    IOLockUnlock(gIopAsLock);
    if (displaced.surface) { OSAddAtomic64(1, &gCntIopAsyncDrop); nvIopDropFrame(&displaced); }
    return true;
}
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)
{
    if (!gIopFlip || !fb || !s || head >= kNvMaxFB || !fb->metaCast("NVRMNVDAFramebuffer") || !nvIopLockReady()) return false;
    NVAccel *a = NVAccel::forDisplay(); NVRetain retained(a);
    if (!a || a->fCandidate || s->getPixelFormat() != NV_IOP_FMT_BGRA || !nvFamLock(a, true)) return false;
    NVFlipView view;
    if (nvAccelIopCacheGet(s, &view)) OSAddAtomic64(1, &gCntIopFlipHit);
    else {
        void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej(a, s, 0);
        NVResource *r = cache ? OSDynamicCast(NVResource, *(OSObject **)((unsigned char *)cache + 0x20)) : nullptr;
        NVVidMemory *m = r ? nvres_vidmem(r) : nullptr;
        if (m && m->fBacking) {
            view.res = r; r->retain(); view.allocation = m->fBacking; view.allocation->retain();
            nvAccelIopCachePut(s, r, m->fBacking);
        }
        if (cache) ((OSObject *)cache)->release();
        OSAddAtomic64(1, &gCntIopFlipMiss);
    }
    NVVidMemory *vm = view.res ? nvres_vidmem(view.res) : nullptr;
    const volatile unsigned short *valid = view.res ? *(unsigned short **)((unsigned char *)view.res + kNvResValid) : nullptr;
    NVVramLease lease;
    bool ok = vm && vm->fBacking == view.allocation && !view.allocation->fOwner->fDirect &&
        valid && nvAccelIopVramFlippable((valid[6] & 1u) != 0,
            nvAccelPluginWrote(view.res->fPluginWroteCookie, view.allocation->fReceipt.allocationCookie)) &&
        view.allocation->acquire(NVLeaseUse::Present, &lease);
    nvFamUnlock(a);
    if (!ok) return nvAccelIopFlipNo(4);
    const uint64_t w = s->getWidth(), h = s->getHeight(), pitch = s->getBytesPerRow();
    if (!w || !h || w > UINT32_MAX || h > UINT32_MAX || pitch > UINT32_MAX ||
        pitch < 4 * w || h * pitch > view.allocation->fReceipt.actualSize) return nvAccelIopFlipNo(6);
    NVRMFlipRequest request = {};
    request.version = NVRM_FLIP_ABI_VERSION; request.kapiMemory = view.allocation->fReceipt.handle;
    request.allocationCookie = view.allocation->fReceipt.allocationCookie;
    request.width = (unsigned)w; request.height = (unsigned)h; request.pitch = (unsigned)pitch;
    if (gIopAsync && pipe) return nvAccelIopAsyncStart((NVDisplayPipe *)pipe, fb, head, s, &request, &lease);
    if (!nvSurfWaitReady(s->getSurfaceID())) return false;
    const OSSymbol *symbol = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn result = symbol ? fb->callPlatformFunction(symbol, false, &request, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (symbol) symbol->release();
    if (result != kIOReturnSuccess) return nvAccelIopFlipNo(7);
    ((OSObject *)s)->retain(); nvIopAdoptFront(head, s, lease.take());
    OSAddAtomic64(1, &gCntIopFlip);
    return true;
}
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS
{
    unsigned v = 0;
    NVAccel *a = NVAccel::forDisplay(); NVRetain retained(a);
    IOService *fb = a ? a->fFB : nullptr;
    IOService *nub = fb ? fb->getProvider() : nullptr;
    struct NVRMDisplayPointers displayPtrs = {};
    displayPtrs.version = NVRM_DISPLAY_PTRS_ABI_VERSION;
    if (nub) {
        const OSSymbol *displayPtrsFn = OSSymbol::withCString(NVRM_DISPLAY_PTRS_FN);
        if (displayPtrsFn) { nub->callPlatformFunction(displayPtrsFn, false, &displayPtrs, NULL, NULL, NULL); displayPtrsFn->release(); }
    }
    struct NvKmsKapiFunctionsTable *k = (struct NvKmsKapiFunctionsTable *)displayPtrs.kapi;
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    h.version = NVRM_GPUVA_ABI_VERSION;
    const OSSymbol *handles = OSSymbol::withCStringNoCopy(NVRM_GPUVA_FN_HANDLES);
    const bool displayHandles = fb && handles && fb->callPlatformFunction(handles, false, &h, nullptr, nullptr, nullptr) == kIOReturnSuccess;
    if (handles) handles->release();
    if (k && k->getCRC32 && gCrcHead >= 0 && gCrcHead < 8 && displayHandles && h.kapiDevice) {
        struct NvKmsKapiCrcs c; nvSVZero(&c, sizeof c);
        if (k->getCRC32((struct NvKmsKapiDevice *)h.kapiDevice, (NvU32)gCrcHead, &c))
            v = c.compositorCrc32.supported ? c.compositorCrc32.value : (c.outputCrc32.supported ? c.outputCrc32.value : 0);
    }
    int out = (int)v;
    return sysctl_handle_int(oidp, &out, 0, req);
}
static bool nvAccelIopHome(IOService *fb, unsigned head)
{
    if (!fb || head >= kNvMaxFB) return false;
    if (!gIopFlipped[head] || !fb->metaCast("NVRMNVDAFramebuffer")) return true;
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION; f.flags = 1u;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    OSAddAtomic64(1, &gCntIopFlipHome);
    // Busy: the flip before HOME is still landing and HOME was not submitted, so the panel is not home yet and the
    // next copied frame tries again. Clearing the flag here left every later copy offscreen behind that flip.
    if (rr != kIOReturnSuccess) return false;
    gIopFlipped[head] = false;
    return true;
}
static bool nvAccelSurfCopyReady(IOSurface *s, unsigned head)
{
    NVAccel *a = NVAccel::forDisplay(); NVRetain retained(a);
    if (!s || !a || head >= kNvMaxFB || !nvFamLock(a, true)) return false;
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej(a, s, 0);
    OSObject *context = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, context);
    const volatile unsigned short *valid = res ? *(unsigned short *volatile *)((unsigned char *)res + kNvResValid) : nullptr;
    bool ready = res && valid && (valid[0] & 1u) && !res->requirePageoff();
    if (!ready && res && !a->fCandidate) {
        bool published = false; res->pageoff(nullptr, true, &published, 0);
        ready = published && valid && (valid[0] & 1u);
    }
    if (ready) nvSVPend(s->getSurfaceID(), 0, res);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
    if (!ready) OSAddAtomic64(1, &gCntIopCopySkip);
    return ready;
}

// nmflatskip: 64 samples of the surface's system-memory copy. Judged only when that copy is the newest one (no page-off
// pending); a surface whose newest pixels are still in VRAM is never called flat, because its old system copy says nothing.
static bool nvAccelFlatHeld(NVDisplayPipe *pipe, unsigned head, IOSurface *s)
{
    if (s->getPixelFormat() != NV_IOP_FMT_BGRA) { gFlat[head].held = 0; return false; }
    const unsigned id = s->getSurfaceID();
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || P < 4 * W || (P & 3) || W > 16384 || H > 16384) return false;
    if (nvSVPend(id, 2, nullptr)) { gFlat[head].held = 0; return false; }
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    if (!md) return false;
    const volatile unsigned int *src = (const volatile unsigned int *)pipe->iopMapSurface(id, md, H * P);
    if (!src) return false;
    uint32_t smp[64];
    const unsigned long spr = (unsigned long)(P / 4);
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++)
        smp[i * 8 + j] = src[(unsigned long)((H * (2 * i + 1)) / 16) * spr + (unsigned long)((W * (2 * j + 1)) / 16)];
    return nmh_flat_hold(&gFlat[head], nmh_samples_flat(smp, 64), gNmFlatSkip) != 0;
}

// ---- hybrid laptops (nm_hybrid.h) ---------------------------------------------------------------------------------------
// Shadow of the previous frame sent to another GPU's panel, one per head: ordinary cached memory, so finding what changed
// costs reads, and the write-combined scan-out gets stores only for those pixels.
static struct { struct nmh_shadow sh; unsigned long long bytes; } gHyShadow[kNvMaxFB];
// import slots: the memory descriptor behind each slot is held (wired, retained) while the other GPU's table maps its pages
static IOMemoryDescriptor *gHyMd[kNvMaxFB][NMH_SLOTS];
static NVVramAllocation *gHyBacking[kNvMaxFB][NMH_SLOTS];
static unsigned gHyMdId[kNvMaxFB][NMH_SLOTS];
static bool gHyOnSlot[kNvMaxFB];      // the panel's plane is on an imported surface, not the boot surface
static unsigned gHyFailLogs;

static void nvAccelHybridRelease(unsigned head, int slot)
{
    if (gHyMd[head][slot]) { gHyMd[head][slot]->complete(); gHyMd[head][slot]->release(); gHyMd[head][slot] = nullptr; }
    gHyMdId[head][slot] = 0;
    nvDropPresentLease(gHyBacking[head][slot]); gHyBacking[head][slot] = nullptr;
}

// The copy path writes into the boot surface, so the plane has to be there first, and the shadow no longer describes it.
static bool nvAccelHybridHome(IOService *fb, unsigned head)
{
    if (!fb || fb->metaCast("NMIntelFB") || head >= kNvMaxFB) return false;
    if (!gHyOnSlot[head]) return true;
    NMHybridReq q; nvSVZero(&q, sizeof q);
    q.version = NMH_ABI_VERSION; q.op = NMH_OP_HOME;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NMH_FN_IMPORT);
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr == kIOReturnSuccess) gHyOnSlot[head] = false;
    nmh_shadow_invalidate(&gHyShadow[head].sh);
    if (rr != kIOReturnSuccess && gHyFailLogs < 8) { gHyFailLogs++; ALOG("hybrid: HOME refused (0x%x) head %u", (unsigned)rr, head); }
    return rr == kIOReturnSuccess;
}

// Level 4: the copy engine writes the frame, which is still only in the GPU's memory, into a buffer the panel driver owns. The
// order of the steps, and where the accelerator lock is held, is nm_hybrid.h's nmh_dma_frame: the GPU is waited for with no
// lock, the lock covers the resource lookup and the copy, and it is released before the flip, whose wait for the next vertical
// blank (up to 40 ms) would otherwise stall every other WindowServer thread that needs the accelerator. The lock is taken and
// given back by this function itself (nvFamLock / nvFamUnlock), so no lock-owner bookkeeping is needed, as there is for a
// page-off that the family calls with the lock already held.
struct NVHyDmaCtx {
    NVDisplayPipe *pipe; IOService *fb; IOSurface *s; const OSSymbol *sym; NVResource *res; NVVidMemory *vm; NVRMRmHandles h;
    NMHybridReq q; unsigned long long len; unsigned id; void *cache; NVAccel *accel;
};
static int hyDmaRender(void *c) { NVHyDmaCtx *x = (NVHyDmaCtx *)c; return nvSurfWaitReady(x->id) ? 0 : -1; }
static int hyDmaLockLookup(void *c)
{
    NVHyDmaCtx *x = (NVHyDmaCtx *)c;
    if (!nvFamLock(x->accel, true)) return -1;
    x->cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)x->accel, (void *)x->s, 0);
    OSObject *ctx = x->cache ? *(OSObject *volatile *)((unsigned char *)x->cache + 0x20) : nullptr;
    x->res = OSDynamicCast(NVResource, ctx);
    x->vm = x->res ? nvres_vidmem(x->res) : nullptr;
    struct NVRMVramRequest sq; nvSVZero(&sq, sizeof sq); sq.version = NVRM_VRAM_ABI_VERSION;
    const IOReturn sr = x->accel->askFramebufferForScanout(&sq, x->fb);
    // the copy is of what is still only in the GPU's memory; nvSVPend was true when the frame was chosen
    const int why = (x->vm && x->vm->handle() && !x->vm->fScanout && sr == kIOReturnSuccess && x->vm->fOwner->handles(&x->h))
        ? nmh_dma_eligible(gNmHybrid, 1, x->s->getPixelFormat(), (unsigned)x->s->getWidth(), (unsigned)x->s->getHeight(), (unsigned)x->s->getBytesPerRow(),
                           sq.width, sq.height, sq.pitch, x->vm->capacity(), sq.actualSize)
        : NMH_E_NOTVRAM;
    if (why == NMH_OK) return 0;
    if (x->cache) { ((OSObject *)x->cache)->release(); x->cache = nullptr; }
    nvFamUnlock(x->accel);
    return -1;
}
static void hyDmaUnlock(void *c)
{
    NVHyDmaCtx *x = (NVHyDmaCtx *)c;
    if (x->cache) { ((OSObject *)x->cache)->release(); x->cache = nullptr; }
    nvFamUnlock(x->accel);
}
static int hyDmaAcquire(void *c, int *idx)
{
    NVHyDmaCtx *x = (NVHyDmaCtx *)c;
    nvSVZero(&x->q, sizeof x->q);
    x->q.version = NMH_ABI_VERSION; x->q.op = NMH_OP_ACQUIRE; x->q.flags = NMH_REQ_DEVICE_WRITES;
    IOReturn rr = x->fb->callPlatformFunction(x->sym, false, &x->q, nullptr, nullptr, nullptr);
    if (rr != kIOReturnSuccess || !x->q.pages || !x->q.npages || (unsigned long long)x->q.npages * 4096 < x->len) return -1;
    *idx = x->q.slot;
    return 0;
}
static int hyDmaCopy(void *c, int idx)
{
    NVHyDmaCtx *x = (NVHyDmaCtx *)c; (void)idx;
    struct NVRMSurfPageoff p; nvSVZero(&p, sizeof p);
    p.version = NVRM_SS_RM_VERSION; p.status = NVRM_SS_OFF; p.kapiDevice = x->h.kapiDevice; p.kapiMemory = x->vm->handle();
    p.pages = x->q.pages; p.pageCount = (x->len + 4095) >> 12; p.length = x->len;
    IOService *nvrm = nvAccelNVRM(x->pipe->fAccel);
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
    if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &p, nullptr, nullptr, nullptr);
    if (sym) sym->release();
    if (p.status == 0) return 0;
    if (p.status == 0x65u) { OSAddAtomic64(1, &gCntHyDmaTimeout); return 2; }
    return 1;
}
static int hyDmaPresent(void *c, int idx)
{
    NVHyDmaCtx *x = (NVHyDmaCtx *)c;
    x->q.op = NMH_OP_PRESENT; x->q.slot = idx;
    return x->fb->callPlatformFunction(x->sym, false, &x->q, nullptr, nullptr, nullptr) == kIOReturnSuccess ? 0 : -1;
}
static void hyDmaAbort(void *c, int idx)
{
    NVHyDmaCtx *x = (NVHyDmaCtx *)c;
    x->q.op = NMH_OP_ABORT; x->q.slot = idx; x->fb->callPlatformFunction(x->sym, false, &x->q, nullptr, nullptr, nullptr);
}
static void hyDmaQuarantine(void *c, int idx)
{
    NVHyDmaCtx *x = (NVHyDmaCtx *)c;
    x->q.op = NMH_OP_QUARANTINE; x->q.slot = idx; x->fb->callPlatformFunction(x->sym, false, &x->q, nullptr, nullptr, nullptr);
}
static bool nvAccelHybridDma(NVDisplayPipe *pipe, IOService *fb, unsigned head, IOSurface *s)
{
    if (!pipe || !fb || !s || head >= kNvMaxFB || fb->metaCast("NVRMNVDAFramebuffer") || fb->metaCast("NMIntelFB") ||
        !pipe->fAccel || pipe->fAccel->fCandidate) return false;
    NVHyDmaCtx x; nvSVZero(&x, sizeof x);
    x.pipe = pipe; x.fb = fb; x.s = s; x.id = s->getSurfaceID();
    x.len = (unsigned long long)s->getBytesPerRow() * s->getHeight();
    x.accel = NVAccel::resolve(pipe->fAccel);
    if (!x.accel || !nvSVPend(x.id, 2, nullptr)) return false;           // the newest pixels are not only in the GPU's memory: level 3
    x.sym = OSSymbol::withCStringNoCopy(NMH_FN_IMPORT);
    if (!x.sym) return false;
    struct nmh_dma_ops ops = { &x, hyDmaRender, hyDmaLockLookup, hyDmaUnlock, hyDmaAcquire, hyDmaCopy, hyDmaPresent, hyDmaAbort, hyDmaQuarantine };
    const int rc = nmh_dma_frame(&ops);
    x.sym->release();
    const bool done = rc == NMH_DMA_DONE;
    if (!done && rc != NMH_DMA_NOT_ELIGIBLE && gHyFailLogs < 8) { gHyFailLogs++; ALOG("hybrid: fallback to copy: copy-engine frame not shown (step result %d)", rc); }
    if (done) {
        gHyOnSlot[head] = true;
        nmh_shadow_invalidate(&gHyShadow[head].sh);
        if (OSAddAtomic64(1, &gCntHyDma) == 0) ALOG("hybrid: first frame written by the copy engine into the panel's own buffer");
    }
    return done;
}

// Level 3: the frame is written into a buffer the panel driver owns (cached memory), and the plane is flipped to it.
// Returns true when the frame is on its way to the screen, false when the caller must copy the old way.
static bool nvAccelHybridOwned(IOService *fb, unsigned head, const volatile unsigned int *src, unsigned w, unsigned h,
                               unsigned pitch, bool from10, unsigned dpitch)
{
    if (!fb || head >= kNvMaxFB || fb->metaCast("NMIntelFB")) return false;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NMH_FN_IMPORT);
    if (!sym) return false;
    struct Guard { const OSSymbol *s; ~Guard() { s->release(); } } guard = { sym };
    NMHybridReq q; nvSVZero(&q, sizeof q);
    q.version = NMH_ABI_VERSION; q.op = NMH_OP_ACQUIRE;
    IOReturn rr = fb->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
    if (rr != kIOReturnSuccess || !q.kva || q.pitch != dpitch) return false;
    const int idx = q.slot;
    struct nmh_stats st;
    if (nmh_present(nullptr, src, w, h, pitch, from10 ? 1 : 0, (volatile uint32_t *)q.kva, dpitch, &st)) {
        q.op = NMH_OP_ABORT; q.slot = idx; fb->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
        return false;
    }
    q.op = NMH_OP_PRESENT; q.slot = idx;
    rr = fb->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
    if (rr != kIOReturnSuccess) {
        q.op = NMH_OP_ABORT; q.slot = idx; fb->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
        gHyOnSlot[head] = true;     // the plane may be on a buffer: the copy that follows goes home first
        if (gHyFailLogs < 8) { gHyFailLogs++; ALOG("hybrid: fallback to copy: %s (owned buffer, 0x%x)", nmh_why_name(q.result ? q.result : NMH_E_STUCK), (unsigned)rr); }
        return false;
    }
    gHyOnSlot[head] = true;
    nmh_shadow_invalidate(&gHyShadow[head].sh);        // the boot surface no longer holds the last frame
    return true;
}

// Scan the composited surface out in place. Returns true when the frame is dealt with (shown, or deliberately held back),
// false when the caller must copy it.
// Can the integrated GPU's display engine read this frame out of the NVIDIA GPU's BAR1 window? (nm_hybrid_p2p.h) The facts come from the PCI
// devices; the answer is cached per head and logged once. Refused unless the boot-arg allows it.
static bool nvAccelP2pAllowed(IOService *fb, unsigned head, unsigned long long frame)
{
    static int verdict[kNvMaxFB]; static bool known[kNvMaxFB];
    if (head >= kNvMaxFB) return false;
    if (!known[head]) {
        struct nmp_topo t; nvSVZero(&t, sizeof t);
        IORegistryEntry *prov = fb->getProvider();
        IORegistryEntry *par = prov ? prov->getParentEntry(gIOServicePlane) : nullptr;
        IOPCIDevice *ipci = OSDynamicCast(IOPCIDevice, prov);
        if (ipci && par) {
            if (OSIterator *it = par->getChildIterator(gIOServicePlane)) {
                while (OSObject *o = it->getNextObject()) {
                    IOPCIDevice *c = OSDynamicCast(IOPCIDevice, o);
                    if (c && c->getBusNumber() == ipci->getBusNumber() && c->getDeviceNumber() == 0 && c->getFunctionNumber() == 0) {
                        t.host_vendor = c->configRead16(kIOPCIConfigVendorID); t.host_device = c->configRead16(kIOPCIConfigDeviceID); }
                }
                it->release();
            }
        }
        NVAccel *a = NVAccel::forDisplay(); NVRetain retained(a);
        if (a && a->fPCI) {
            const uint32_t b1 = a->fPCI->configRead32(0x14);          // BAR1: the framebuffer window; bit 3 prefetchable, bits 2:1 == 2 for 64-bit
            t.dgpu_reachable = 1;
            t.bar1_64bit_prefetch = ((b1 & 0x6u) == 0x4u) && (b1 & 0x8u);
            t.bar1_bytes = NVRM_VRAM_BAR1_BUDGET;                      // the part of the window the driver hands out
        }
        t.igpu_dma_translated = 0;                                     // the system-memory import already relies on untranslated DMA
        verdict[head] = nmp_decide(gNmHybridP2P, &t, frame);
        known[head] = true;
        ALOG("hybrid: zero-copy from VRAM, head %u: %s (host bridge %04x:%04x)", head, nmp_name(verdict[head]), t.host_vendor, t.host_device);
    }
    return nmp_allowed(verdict[head]);
}

static bool nvAccelHybridImport(IOService *fb, unsigned head, IOSurface *s, NVVramAllocation *vram)
{
    if (!fb || !s || head >= kNvMaxFB || fb->metaCast("NVRMNVDAFramebuffer") || fb->metaCast("NMIntelFB")) return false;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NMH_FN_IMPORT);
    if (!sym) return false;
    struct Guard { const OSSymbol *s; ~Guard() { s->release(); } } guard = { sym };
    const unsigned id = s->getSurfaceID();
    NMHybridReq q; nvSVZero(&q, sizeof q);
    q.version = NMH_ABI_VERSION; q.op = NMH_OP_QUERY; q.surfaceId = id; q.format = s->getPixelFormat();
    q.width = (unsigned)s->getWidth(); q.height = (unsigned)s->getHeight(); q.pitch = (unsigned)s->getBytesPerRow();
    IOReturn rr = fb->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
#define HYFAIL(why) do { OSAddAtomic64(1, &gCntHyImportFail); \
        if (gHyFailLogs < 8) { gHyFailLogs++; ALOG("hybrid: fallback to copy: %s (surface 0x%x, 0x%x)", nmh_why_name(why), id, (unsigned)rr); } \
        return false; } while (0)
    if (rr != kIOReturnSuccess) HYFAIL(q.result ? q.result : NMH_E_PROVIDER);
    const int slot = q.slot;
    if (slot < 0 || slot >= NMH_SLOTS) HYFAIL(NMH_E_BUSY);
    if (q.evictedId) for (int i = 0; i < NMH_SLOTS; i++) if ((gHyMd[head][i] || gHyBacking[head][i]) && gHyMdId[head][i] == q.evictedId) nvAccelHybridRelease(head, i);
    // the surface's pixels must be final: the GPU done with them, and in system memory where the other GPU reads them
    if (!nvSurfWaitReady(id)) HYFAIL(NMH_E_STUCK);
    const unsigned long long len = (unsigned long long)q.pitch * q.height;
    // zero copy: the frame stays in the NVIDIA GPU's memory and the table entries point at its BAR1 window. The GPU's memory is the newest
    // copy, so there is nothing to page out or flush; a frame that does not fit the window or sits off a page boundary takes the paths below.
    NVVramLease imported;
    const bool p2p = vram && !vram->fOwner->fDirect && vram->fReceipt.phys &&
        !(vram->fReceipt.phys & 4095ull) && len <= vram->fReceipt.actualSize && nvAccelP2pAllowed(fb, head, len) &&
        vram->acquire(NVLeaseUse::Present, &imported);
    if (!p2p && !nvAccelSurfCopyReady(s, head)) return false;
    IOMemoryDescriptor *md = p2p ? nullptr : s->getMemoryDescriptor();
    if (!p2p && (!md || md->getLength() < len)) HYFAIL(NMH_E_PAGES);
    if (q.needMap) {
        nvAccelHybridRelease(head, slot);
        const unsigned npages = (unsigned)nmh_pages_for(q.pitch, q.height);
        unsigned long long *pa = (unsigned long long *)IOMalloc((size_t)npages * sizeof *pa);
        if (!pa) HYFAIL(NMH_E_PAGES);
        if (!p2p && md->prepare() != kIOReturnSuccess) { IOFree(pa, (size_t)npages * sizeof *pa); HYFAIL(NMH_E_PAGES); }
        bool ok = true;
        for (unsigned i = 0; i < npages && ok; i++) {
            if (p2p) { if (i == 0 && !nmp_pages(vram->fReceipt.phys, vram->fReceipt.actualSize, len, pa, npages)) ok = false; continue; }
            IOByteCount seg = 0;
            addr64_t a = md->getPhysicalSegment((IOByteCount)i * 4096, &seg, kIOMemoryMapperNone);
            if (!a || seg < 4096 || (a & 4095)) ok = false; else pa[i] = a;
        }
        if (ok) { q.op = NMH_OP_MAP; q.npages = npages; q.pages = pa; rr = fb->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr); ok = rr == kIOReturnSuccess; }
        IOFree(pa, (size_t)npages * sizeof *pa);
        if (!ok) { if (md) md->complete(); HYFAIL(q.result ? q.result : NMH_E_PAGES); }
        if (md) { md->retain(); gHyMd[head][slot] = md; } else gHyMd[head][slot] = nullptr;
        gHyMdId[head][slot] = id;
        if (p2p) gHyBacking[head][slot] = imported.take();
    }
    // the plugin wrote these pixels with the CPU; the other GPU reads memory, so they have to have left the CPU's caches (not needed for VRAM)
    if (md) md->performOperation(kIOMemoryIncoherentIOFlush, 0, (IOByteCount)len);
    if (p2p && OSAddAtomic64(1, &gCntHyP2p) == 0) ALOG("hybrid: first frame scanned out of the NVIDIA GPU's memory (zero copy, head %u)", head);
    q.op = NMH_OP_FLIP; q.slot = slot;
    rr = fb->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
    if (rr != kIOReturnSuccess) { gHyOnSlot[head] = true; HYFAIL(q.result ? q.result : NMH_E_STUCK); }   // HOME on the copy that follows
    gHyOnSlot[head] = true;
    OSAddAtomic64(1, &gCntHyFlips);
    return true;
#undef HYFAIL
}

static void nvAccelDrainPipe(NVDisplayPipe *pipe)
{
    if (!pipe || !gIopAsLock) return;
    for (unsigned head = 0; head < kNvMaxFB; ++head) {
        NVIopAsync *e = &gIopAs[head];
        IOLockLock(gIopAsLock);
        bool ours = e->pipe == pipe;
        if (ours) e->stopping = true;
        while (ours && e->busy) {
            if (e->call && thread_call_cancel(e->call)) {
                e->busy = 0;
                break;
            }
            IOLockSleep(gIopAsLock, (event_t)&e->busy, THREAD_UNINT);
        }
        if (!ours) { IOLockUnlock(gIopAsLock); continue; }
        NVIopAsyncFrame current = e->current, next = e->next;
        e->current = {}; e->next = {}; e->busy = 0;
        IOService *fb = e->fb; NVVramOwner *owner = e->owner;
        struct thread_call *call = e->call;
        e->fb = nullptr; e->pipe = nullptr; e->owner = nullptr; e->call = nullptr;
        e->stopping = false;
        IOLockUnlock(gIopAsLock);
        nvIopDropFrame(&current); nvIopDropFrame(&next);
        if (call) {
            thread_call_cancel(call);
            while (!thread_call_free(call)) { thread_call_cancel(call); IOSleep(1); }
        }
        if (fb && owner) {
            IOLockLock(gIopAsLock);
            if (owner->retireScanout(head, fb, (OSObject *)gIopFlipFront[head], (OSObject *)gIopFlipPrev[head],
                    gIopFrontBacking[head], gIopPrevBacking[head])) {
                gIopFlipFront[head] = gIopFlipPrev[head] = nullptr;
                gIopFrontBacking[head] = gIopPrevBacking[head] = nullptr; gIopFlipped[head] = false;
            }
            IOLockUnlock(gIopAsLock);
            fb->release();
        }
        if (owner) owner->unborrow();
        pipe->release();
    }
}
static void nvAccelDrainPresent(NVAccel *accel)
{
    if (gIopAsLock) for (unsigned h = 0; h < kNvMaxFB; ++h) {
        IOLockLock(gIopAsLock);
        NVDisplayPipe *pipe = gIopAs[h].owner == accel->fOwner ? gIopAs[h].pipe : nullptr;
        if (pipe) pipe->retain();
        IOLockUnlock(gIopAsLock);
        if (pipe) { nvAccelDrainPipe(pipe); pipe->release(); }
    }
    if (gIopAsLock && accel->fOwner) for (unsigned h = 0; h < kNvMaxFB; ++h) {
        IOLockLock(gIopAsLock);
        if (gIopFrontBacking[h] && gIopFrontBacking[h]->fOwner == accel->fOwner && accel->fFBs[h] &&
            accel->fOwner->retireScanout(h, accel->fFBs[h], (OSObject *)gIopFlipFront[h], (OSObject *)gIopFlipPrev[h],
                gIopFrontBacking[h], gIopPrevBacking[h])) {
            gIopFlipFront[h] = gIopFlipPrev[h] = nullptr; gIopFrontBacking[h] = gIopPrevBacking[h] = nullptr; gIopFlipped[h] = false;
        }
        IOLockUnlock(gIopAsLock);
    }
    if (gIopFlipCacheLock) for (unsigned i = 0; i < 8; ++i) {
        NVIopFlipCache old = {};
        IOSimpleLockLock(gIopFlipCacheLock);
        if (gIopFlipCache[i].allocation && gIopFlipCache[i].allocation->fOwner == accel->fOwner) {
            old = gIopFlipCache[i]; gIopFlipCache[i] = {};
        }
        IOSimpleLockUnlock(gIopFlipCacheLock);
        if (old.s) ((OSObject *)old.s)->release();
        if (old.res) old.res->release();
        if (old.allocation) old.allocation->release();
    }
    if (accel->fDM) {
        const unsigned count = accel->fDM->getFramebufferCount();
        for (unsigned i = 0; i < count && i < 16; ++i) {
            NVDisplayPipe *pipe = OSDynamicCast(NVDisplayPipe, *(OSObject **)((unsigned char *)accel->fDM + 0x88 + i * 8));
            if (pipe) { pipe->displayModeWillChange(); pipe->iopDropSources(); }
        }
    }
}

bool NVMemoryMap::init(IOGraphicsAccelerator2 *a, IOAccelTask *t, IOAccelMemory *m, unsigned int options)
{
    NVVidMemory *vm = OSDynamicCast(NVVidMemory, m);
    NVAccel *accel = OSDynamicCast(NVAccel, a);
    NVVramOwner *owner = vm ? vm->fOwner : accel ? accel->fOwner : nullptr;
    if (!owner || !owner->borrow()) return false;
    fOwner = owner; fMem = m;
    // The owner is bound before base init can reserve VA, even if no physical backing exists yet.
    return IOAccelMemoryMap::init(a, t, m, options);
}
bool NVGpuVaRecord::unmap(bool releaseVa)
{
    IOLockLock(fOwner->fLock);
    if (fBusy) { IOLockUnlock(fOwner->fLock); return false; }
    if (!fRequest.hVirt) { IOLockUnlock(fOwner->fLock); return true; }
    fBusy = true;
    NVRMGpuVaRequest q = fRequest;
    q.hMemory = fMapped ? fRequest.hMemory : 0;
    q.flags = releaseVa ? 0 : NVRM_GPUVA_FLAG_UNMAP_ONLY;
    IOLockUnlock(fOwner->fLock);
    const bool ok = fOwner->call(NVRM_GPUVA_FN_FREE, &q) == kIOReturnSuccess && q.status == 0;
    const bool unmapped = !q.hMemory;
    IOLockLock(fOwner->fLock);
    if (unmapped) { fMapped = false; fRequest.hMemory = 0; }
    if (ok && releaseVa) { fRequest.hVirt = 0; fRequest.gpuva = 0; }
    IOLockUnlock(fOwner->fLock);
    if (unmapped) { fLease.reset(); fKey = {}; }
    IOLockLock(fOwner->fLock); fBusy = false; IOLockUnlock(fOwner->fLock);
    return ok;
}
void NVGpuVaRecord::free() { fLease.reset(); if (fOwner) fOwner->release(); OSObject::free(); }
static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map, unsigned long long len, unsigned long long align)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
    if (!mm || !mm->fOwner || !len) return 0;
    if (mm->fRecord) return mm->fRecord->fRequest.gpuva;
    NVVramOwner *owner = mm->fOwner;
    if (!owner->enter()) return 0;
    NVGpuVaRecord *r = OSTypeAlloc(NVGpuVaRecord);
    bool ok = r && r->init();
    IOLockLock(owner->fLock);
    ok = ok && owner->fState.phase == NVOwnerPhase::Active && owner->ensureDeviceLocked() &&
         owner->fState.reservations != UINT32_MAX;
    if (ok) {
        r->fRequest.version = NVRM_GPUVA_ABI_VERSION; r->fRequest.kapiDevice = owner->fDevice;
        r->fRequest.size = len; r->fRequest.align = align;
        ++owner->fState.reservations;
    }
    IOLockUnlock(owner->fLock);
    if (!ok) { if (r) r->release(); owner->leave(); return 0; }
    ok = owner->call(NVRM_GPUVA_FN_ALLOC, &r->fRequest) == kIOReturnSuccess &&
         r->fRequest.status == 0 && r->fRequest.hVirt && r->fRequest.gpuva;
    IOLockLock(owner->fLock);
    ok = ok && owner->fState.phase == NVOwnerPhase::Active;
    if (r->fRequest.hVirt) {
        r->fOwner = owner; owner->retain();
        r->fNext = owner->fReservations; owner->fReservations = r; r->retain();
        if (ok) mm->fRecord = r;
        else { r->fRetiring = true; owner->fRetirementPending = true; }
    } else {
        --owner->fState.reservations;
    }
    IOLockUnlock(owner->fLock); owner->leave();
    if (!ok) { r->release(); return 0; }
    return r->fRequest.gpuva;
}
static void nvAccelGpuVaFree(IOAccelMemoryMap *map)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
    if (!mm || !mm->fRecord) return;
    NVGpuVaRecord *r = mm->fRecord;
    NVVramOwner *owner = r->fOwner;
    // Failed cleanup remains on the owner's retirement list with the original GPU-mapping lease.
    IOLockLock(owner->fLock); r->fRetiring = true; owner->fRetirementPending = true; IOLockUnlock(owner->fLock);
    mm->fRecord = nullptr;
    owner->scheduleReap(); r->release();
    OSAddAtomic64(1, &gCntMapFree);
}
bool NVMemoryMap::commitIntoGPUPageTable()
{
    NVVidMemory *vm = OSDynamicCast(NVVidMemory, fMem);
    if (!vm || vm->fOwner != fOwner || !vm->fBacking || !fRecord || fRecord->fOwner != fOwner) return false;
    NVGpuVaRecord *r = fRecord;
    if (!fOwner->enter()) return false;
    IOLockLock(fOwner->fLock);
    if (r->fBusy || r->fRetiring || r->fMapped || !r->fRequest.hVirt || r->fRequest.size > vm->capacity()) {
        const bool mapped = !r->fBusy && !r->fRetiring && r->fMapped && nvBackingKeyEqual(r->fKey, vm->fBacking->key());
        IOLockUnlock(fOwner->fLock); fOwner->leave(); return mapped;
    }
    r->fBusy = true;
    NVRMGpuVaRequest q = r->fRequest; q.kapiMemory = vm->handle(); q.isVidmem = 1;
    IOLockUnlock(fOwner->fLock);
    bool ok = vm->fBacking->acquire(NVLeaseUse::GpuMapping, &r->fLease);
    if (ok) ok = fOwner->call(NVRM_GPUVA_FN_MAP, &q) == kIOReturnSuccess && q.status == 0;
    IOLockLock(fOwner->fLock);
    const bool admitted = ok && !r->fRetiring && fOwner->fState.phase == NVOwnerPhase::Active;
    if (ok) {
        r->fRequest = q; r->fMapped = true; r->fKey = vm->fBacking->key();
        if (!admitted) { r->fRetiring = true; fOwner->fRetirementPending = true; }
    }
    IOLockUnlock(fOwner->fLock);
    if (!ok) r->fLease.reset(); else if (admitted) OSAddAtomic64(1, &gCntMapCommit);
    IOLockLock(fOwner->fLock); r->fBusy = false; IOLockUnlock(fOwner->fLock);
    fOwner->leave();
    return admitted;
}
void NVMemoryMap::releaseFromGPUPageTable()
{
    if (fRecord && fRecord->fMapped && fRecord->unmap(false)) OSAddAtomic64(1, &gCntMapRelease);
}
bool NVMemoryMap::redirect(IOAccelMemory *memory)
{
    NVVidMemory *vm = OSDynamicCast(NVVidMemory, memory);
    if (!vm || !vm->fOwner || !vm->fOwner->borrow()) return false;
    if (fRecord && !fRecord->unmap(true)) { vm->fOwner->unborrow(); return false; }
    nvAccelGpuVaFree(this);
    if (!IOAccelMemoryMap::redirect(memory)) { vm->fOwner->unborrow(); return false; }
    NVVramOwner *old = fOwner; fOwner = vm->fOwner; fMem = memory;
    if (old) old->unborrow();
    return true;
}
void NVMemoryMap::free()
{
    nvAccelGpuVaFree(this);
    NVVramOwner *owner = fOwner;
    IOAccelMemoryMap::free();
    if (owner) owner->unborrow();
}

static void nvAccelWriteStamp(int index, unsigned int value)
{
    NVAccel *a = NVAccel::forDisplay(); NVRetain retained(a);
    if (!a || !a->fStamps || index < 0 || index >= 4096) return;
    volatile unsigned int *base = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
    if (!base) return;
    base[index] = value;
}

static unsigned nvSwapProbes;
static void nvAccelResStats(IOAccelResource2 *resource, const char *tag)
{
    NVResource *r = OSDynamicCast(NVResource, resource);
    NVVidMemory *m = r ? nvres_vidmem(r) : nullptr;
    ALOGV("swap %s: resource %p allocation generation %llu CPU publication %llu", tag, r,
          m && m->fBacking ? (unsigned long long)m->fBacking->key().allocationGeneration : 0ull,
          r ? (unsigned long long)r->fCpuPublication : 0ull);
}
bool nvAccelResHasContent(IOAccelResource2 *resource)
{
    NVResource *r = OSDynamicCast(NVResource, resource);
    if (!r) return false;
    NVVidMemory *m = nvres_vidmem(r);
    if (m && m->fBacking && r->fCpuPublication && nvBackingKeyEqual(r->fCpuPublicationKey, m->fBacking->key())) return true;
    // A black completed IOSurface is just as valid as a nonzero one.
    const volatile unsigned short *valid = *(unsigned short *const *)((const unsigned char *)r + kNvResValid);
    return valid && (valid[0] & 1u) && !r->requirePageoff();
}

void nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2)
{
    if (nvSwapProbes >= 6) return;
    nvSwapProbes++;
    ALOG("SWAP PROBE %u -- which argument holds the composited frame?", nvSwapProbes);
    nvAccelResStats(a1, "a1");
    nvAccelResStats(a2, "a2");
}

static IOSurface *nvResourceSurface(NVResource *resource)
{
    void *cache = *(void **)((unsigned char *)resource + kNvResCache);
    IOSurface *s = cache ? *(IOSurface **)((unsigned char *)cache + 0x10) : nullptr;
    if (s) ((OSObject *)s)->retain();
    return s;
}
static bool nvAccelCopyToScanout(IOAccelResource2 *resource)
{
    NVAccel *a = NVAccel::forDisplay(); NVRetain retained(a);
    NVResource *r = OSDynamicCast(NVResource, resource);
    if (!a || !r || r->fAccel != a || !a->fDM || a->fDM->getFramebufferCount() != 1) return false;
    NVDisplayPipe *pipe = OSDynamicCast(NVDisplayPipe, *(OSObject **)((unsigned char *)a->fDM + 0x88));
    unsigned head = 0; IOService *fb = pipe ? a->pipeFramebuffer(pipe, &head) : nullptr;
    NVTransactionGate gate(pipe);
    if (!fb || !gate.admitted || !nvFamLock(a, true)) return false;
    IOSurface *surface = nvResourceSurface(r);
    NVVramLease lease; nv_frame_span source = {};
    NVVidMemory *m = nvres_vidmem(r);
    if (!surface && m && m->fBacking && r->fCpuPublication &&
        nvBackingKeyEqual(r->fCpuPublicationKey, m->fBacking->key()) &&
        m->fBacking->acquire(NVLeaseUse::CpuAccess, &lease)) {
        const uint64_t pitch = nvResPitch((const unsigned char *)r);
        if (pitch <= UINT32_MAX) source = { m->kva(), m->capacity(),
            *(uint16_t *)((char *)r + kNvResWidth), *(uint16_t *)((char *)r + kNvResHeight), (uint32_t)pitch, NV_IOP_FMT_BGRA };
    }
    nvFamUnlock(a);
    NVRetain holdSurface((OSObject *)surface);
    bool sourceCached = false;
    if (surface) {
        if (!nvSurfWaitReady(surface->getSurfaceID()) || !nvAccelSurfCopyReady(surface, head)) return false;
        const uint64_t w = surface->getWidth(), h = surface->getHeight(), pitch = surface->getBytesPerRow();
        IOMemoryDescriptor *md = surface->getMemoryDescriptor();
        if (!md || w > UINT32_MAX || h > UINT32_MAX || pitch > UINT32_MAX) return false;
        source = { pipe->iopMapSurface(surface->getSurfaceID(), md, pitch * h, &sourceCached), md->getLength(),
            (uint32_t)w, (uint32_t)h, (uint32_t)pitch, surface->getPixelFormat() };
    }
    if (nv_frame_validate(&source) != NV_FRAME_OK) return false;
    IOLockLock(pipe->fTransactionLock);
    uint64_t sequence = 0;
    const bool numbered = nvNextGeneration(&pipe->fNextSequence, &sequence);
    if (numbered) pipe->fTransaction.begin(nullptr, 0, sequence, pipe->fFramebufferInfo.epoch);
    IOLockUnlock(pipe->fTransactionLock);
    return numbered && pipe->presentFinal(fb, source, sourceCached) == kIOReturnSuccess;
}
static bool nvAccelFlipTo(IOAccelResource2 *resource)
{
    NVAccel *a = NVAccel::forDisplay(); NVRetain retained(a);
    NVResource *r = OSDynamicCast(NVResource, resource);
    if (!a || a->fCandidate || !r || r->fAccel != a || !a->fFB ||
        !a->fDM || a->fDM->getFramebufferCount() != 1 || !nvFamLock(a, true)) return false;
    IOSurface *s = nvResourceSurface(r);
    nvFamUnlock(a);
    NVRetain holdSurface((OSObject *)s);
    return s && nvAccelIopFlip(a->fFB, 0, s, nullptr);
}

IOReturn NVAccel::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && p1 && fn->isEqualTo(NVACCEL_SS_FN)) {
        if (fCandidate) return nvAccelSurfVram(this, (NVRMSurfShareEsc *)p1, (task_t)p2, (unsigned)(uintptr_t)p3);
        NVVramOwner *owner = fOwner;
        if (!owner || !owner->enter()) return kIOReturnNotReady;
        IOReturn result = nvAccelSurfVram(this, (struct NVRMSurfShareEsc *)p1, (task_t)p2, (unsigned)(uintptr_t)p3);
        owner->leave(); return result;
    }
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

NM_TAHOE_FWD_DEFS
