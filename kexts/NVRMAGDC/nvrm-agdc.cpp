/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "nvrm_off.h"
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>          // clock_get_uptime/nanoseconds_to_absolutetime: the 0x980 rescan throttle
#include <libkern/OSAtomic.h>
#define FBLOG(fmt, ...) IOLog("NVRM-agdc: " fmt "\n", ##__VA_ARGS__)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVRMAGDC, "0.1", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }
#include "AppleGraphicsDeviceControl_re.h"
#include "nvrm-agdc-k5.h"
#include "nvrm-agdc-hybrid.h"
#pragma pack(push, 1)
struct NVAGDCVendorInfo {
    uint32_t version;
    char     vendor[0x20];
    uint32_t vendorID;
    uint32_t vendorClass;
};
struct NVAGDCGPUCapability {
    uint64_t portMask;
    uint64_t portFeatureA;
    uint64_t portFeatureB;
    uint64_t portFeatureC;
    uint32_t countA, countB, countC, countD;
    uint32_t maxFramebuffers;
    IOService *pciDevice;
    IOService *framebuffer[20];
};
#pragma pack(pop)
static_assert(sizeof(NVAGDCVendorInfo) == 0x2c, "AGDCVendorInfo_t is 0x2c");
static_assert(sizeof(NVAGDCGPUCapability) == 0xdc, "AGDCGPUCapability_t is 0xdc");

static int    gAgdcK5 = 0;
static SInt64 gAgdcK5Link = 0, gAgdcK5Caps = 0;
static int    gAgdcK5Why = 0;
static SInt64 gAgdcCmds = 0, gAgdcUnsupported = 0;
static int    gAgdcLastRefused = 0;
static int    gAgdcLogged = 0;
// Slot-table rescans done while answering 0x980 (see nvdaAgdcRescanForReply).
static SInt64 gAgdcRescans = 0;

enum { kAgdcMaxFB = NVRM_AGDC_MAX_SLOTS };
static_assert(kAgdcMaxFB == 5, "keep in sync with NVRM_FB_MAX_HEADS (4 NVIDIA + 1 hybrid)");
static IOLock    *gAgdcLock = nullptr;
static IOService *gAgdcFB[kAgdcMaxFB];
static unsigned  gAgdcNFB = 0;
enum { kAgdcFBMapBytes = 3 + kAgdcMaxFB * 25 + 52 + 1 };
static_assert(kAgdcMaxFB >= 1 && kAgdcMaxFB <= 8, "IOPresentment's map holds 8 (kext 0x31b3), and kAgdcFBMapBytes assumes one digit for n and each slot index");
static char   gAgdcFBMap[kAgdcFBMapBytes] = "never scanned";
static int    gAgdcMaxFBOut = 0;
// The maximum to report: at least the started count, up to the head slots the driver publishes, never above the table.
static inline unsigned nvrmAgdcMaxFramebuffers(unsigned started, int heads, unsigned tableMax)
{
    unsigned want = heads < 1 ? 1u : (unsigned)heads;
    if (want > tableMax) want = tableMax;
    return want > started ? want : started;
}
static int    gAgdcK5Asked = 0, gAgdcK5LinkMask = 0, gAgdcK5CapsMask = 0;
static int    gAgdcK5LastRefusedIdx = -1;

class NVDADeviceControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(NVDADeviceControl)
public:
    IOService *fPci; IOService *fFB;
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                              unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVDADeviceControl, AppleGraphicsDeviceControl)

static IOReturn nvdaK5Answer(IOService *fb, unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out, unsigned long *outSize)
{
    IOService *slot[kAgdcMaxFB] = {};
    unsigned nh = 0;
    if (gAgdcLock) IOLockLock(gAgdcLock);
    nh = gAgdcNFB;
    if (nh == 0 || nh > kAgdcMaxFB) {
        slot[0] = fb;
        nh = 1;
        if (slot[0]) slot[0]->retain();
    } else {
        for (unsigned i = 0; i < nh; i++) {
            slot[i] = gAgdcFB[i];
            if (slot[i]) slot[i]->retain();
        }
    }
    if (gAgdcLock) IOLockUnlock(gAgdcLock);

    uint32_t t[kAgdcMaxFB][NVK5_TIMING_WORDS]; struct nvk5_head h[kAgdcMaxFB];
    for (unsigned i = 0; i < nh; i++) {
        unsigned long tBytes = 0; bool have = false;
        OSObject *o = slot[i] ? slot[i]->copyProperty("NVRMTiming") : NULL; OSData *d = OSDynamicCast(OSData, o);
        if (d) { have = true; tBytes = d->getLength(); bzero(t[i], sizeof t[i]); if (tBytes == sizeof t[i]) memcpy(t[i], d->getBytesNoCopy(), sizeof t[i]); }
        OSSafeReleaseNULL(o);
        h[i].t = have ? t[i] : NULL; h[i].tBytes = tBytes;
    }
    int why = 0; int32_t idx = -1;
    int r = nvk5_answer_heads(cmd, in, inSize, out, outSize ? *outSize : 0, h, nh, &why, &idx);
    const UInt32 bit = (idx >= 0 && idx < 32) ? (1u << idx) : 0;
    if (bit) OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5Asked);
    IOReturn ret;
    if (r == NVK5_R_LINK) {
        OSAddAtomic64(1, &gAgdcK5Link); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5LinkMask);
        ret = kIOReturnSuccess;
    } else if (r == NVK5_R_CAPS) {
        OSAddAtomic64(1, &gAgdcK5Caps); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5CapsMask);
        ret = kIOReturnSuccess;
    } else if (r == NVK5_R_UNSUPPORTED) {
        ret = kIOReturnUnsupported;
    } else {
        if (r == NVK5_R_NOTFOUND && idx >= 0 && (unsigned)idx < nh && !slot[idx]) why = 0x30;
        gAgdcK5Why = why; gAgdcK5LastRefusedIdx = idx;
        if (gAgdcLogged < 40) { gAgdcLogged++; FBLOG("AGDC K5: command %#x index %d NOT answered (why %#x, %u slot(s)) [%d/40]", cmd, idx, why, nh, gAgdcLogged); }
        ret = r == NVK5_R_BADARG ? kIOReturnBadArgument : kIOReturnNotFound;
    }
    for (unsigned i = 0; i < nh; i++) OSSafeReleaseNULL(slot[i]);
    return ret;
}

static void nvdaAgdcScan(IOService *pci);
// WindowServer reads AGDC command 0x980 when it (re)starts and builds one display pipe per entry in the reply. The
// slot table used to be scanned only once, when AGDC came up, so a framebuffer that started later -- a monitor
// plugged in after boot -- was never in the answer: WindowServer then built a single display pipe, the second
// display's composited frames had no pipe to go to, and the panel showed only its cursor while `screencapture -D 2`
// showed a full desktop (iop_ok_h0 grew, iop_ok_h1 stayed 0). Re-scan before answering, at most once a second and
// logging only when the count changes, so the reply is never stale. This runs in the vendor-call context, which
// holds no registry lock, and nvdaAgdcScan only matches services and reads their properties.
static void nvdaAgdcRescanForReply(IOService *pci)
{
    if (!pci) return;
    static uint64_t last = 0;
    uint64_t now = 0; clock_get_uptime(&now);
    uint64_t per = 0; nanoseconds_to_absolutetime(1000000000ull, &per);
    if (gAgdcLock) IOLockLock(gAgdcLock);
    const bool skip = (last && now - last < per);
    if (!skip) last = now;
    const unsigned before = gAgdcNFB;
    if (gAgdcLock) IOLockUnlock(gAgdcLock);
    if (skip) return;
    nvdaAgdcScan(pci);
    OSAddAtomic64(1, &gAgdcRescans);
    if (gAgdcLock) IOLockLock(gAgdcLock);
    const unsigned after = gAgdcNFB;
    if (gAgdcLock) IOLockUnlock(gAgdcLock);
    if (after != before)
        FBLOG("AGDC: 0x980 rescan -> %u framebuffer slot(s) (was %u)", after, before);
}

IOReturn NVDADeviceControl::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                                     unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args)
{
    OSAddAtomic64(1, &gAgdcCmds);
    if (cmd == 1) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCVendorInfo)) return kIOReturnBadArgument;
        NVAGDCVendorInfo *v = (NVAGDCVendorInfo *)out; bzero(v, sizeof *v);
        v->version = 0x30000; strlcpy(v->vendor, "NVIDIA", sizeof v->vendor); v->vendorID = 0x10de; v->vendorClass = 2;
        return kIOReturnSuccess;
    }
    if (cmd == 0x980) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCGPUCapability)) return kIOReturnBadArgument;
        nvdaAgdcRescanForReply(fPci);   // a framebuffer that started after boot has to be in this reply
        NVAGDCGPUCapability *c = (NVAGDCGPUCapability *)out; bzero(c, sizeof *c);
        if (gAgdcLock) IOLockLock(gAgdcLock);
        unsigned n = gAgdcNFB;
        if (n == 0 || n > kAgdcMaxFB) {
            c->portMask = 1ull << 1;
            c->countA = c->countB = c->countC = c->countD = 1;
            c->maxFramebuffers = 1; c->pciDevice = fPci; c->framebuffer[0] = fFB;
        } else {
            for (unsigned i = 0; i < n; i++) { c->framebuffer[i] = gAgdcFB[i]; c->portMask |= 1ull << (1 + i); }
            c->countA = c->countB = c->countC = c->countD = n;
            c->maxFramebuffers = n; c->pciDevice = fPci;
        }
        if (gAgdcLock) IOLockUnlock(gAgdcLock);
        // Boot-arg nvagdcheads=1: report the framebuffer slots that CAN exist (nvfbheads) as the maximum, not only the ones
        // started now. With one display at boot the reply said 1, and macOS then refused the framebuffer a later hot-plug
        // starts (1401 issue 146, "AGDC maxfb=1"). The framebuffer[] list and port mask still name only the started ones.
        {   UInt32 on = 0; int heads = 1;
            if (PE_parse_boot_argn("nvagdcheads", &on, sizeof on) && on) {
                PE_parse_boot_argn("nvfbheads", &heads, sizeof heads);
                const unsigned mx = nvrmAgdcMaxFramebuffers(c->maxFramebuffers, heads, kAgdcMaxFB);
                if (mx != c->maxFramebuffers) { c->maxFramebuffers = mx; }
            } }
        gAgdcMaxFBOut = (int)c->maxFramebuffers;
        return kIOReturnSuccess;
    }
    if (gAgdcK5 && (cmd == NVK5_CMD_LINKCONFIG || cmd == NVK5_CMD_FBCAPEX)) {
        IOReturn k5 = nvdaK5Answer(fFB, cmd, in, inSize, out, outSize);
        if (k5 != kIOReturnUnsupported) return k5;
    }
    OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
    if (gAgdcLogged < 40) { gAgdcLogged++; FBLOG("AGDC: command %#x (in %lu bytes) -> Unsupported [%d/40]", cmd, inSize, gAgdcLogged); }
    return kIOReturnUnsupported;
}

static int gAgdcState = 0; static NVDADeviceControl *gAgdc = nullptr; static thread_call_t gAgdcCall = nullptr; static int gAgdcWant = 0;
static IOService *gAgdcFBRef = nullptr;
static IOService *gAgdcPciRef = nullptr;

static IOService *nvdaFindPciAncestor(IOService *svc)
{
    IOService *p = svc ? svc->getProvider() : nullptr;
    int hops = 0;
    while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
    return (p && p->metaCast("IOPCIDevice")) ? p : nullptr;
}

static bool nvdaReadU64Prop(IOService *s, const char *key, uint64_t *out)
{
    if (!s || !key || !out) return false;
    OSObject *o = s->copyProperty(key);
    OSNumber *n = OSDynamicCast(OSNumber, o);
    const bool ok = (n != nullptr);
    if (ok) *out = (uint64_t)n->unsigned64BitValue();
    OSSafeReleaseNULL(o);
    return ok;
}

static bool nvdaReadU32Prop(IOService *s, const char *key, uint32_t *out)
{
    if (!s || !key || !out) return false;
    OSObject *o = s->copyProperty(key);
    OSNumber *n = OSDynamicCast(OSNumber, o);
    const bool ok = (n != nullptr);
    if (ok) *out = (uint32_t)n->unsigned32BitValue();
    OSSafeReleaseNULL(o);
    return ok;
}

static IOService *nvdaFindNvAccelOnPci(IOService *pci, IOService **outPci)
{
    if (outPci) *outPci = nullptr;
    OSDictionary *m = IOService::serviceMatching("NVAccel");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    OSSafeReleaseNULL(m);
    if (!it) return nullptr;
    IOService *found = nullptr;
    OSObject *o;
    while (!found && (o = it->getNextObject())) {
        IOService *a = OSDynamicCast(IOService, o);
        if (!a || a->isInactive()) continue;
        IOService *p = nvdaFindPciAncestor(a);
        if (!p) continue;
        if (pci && p != pci) continue;
        a->retain();
        found = a;
        if (outPci) *outPci = p;
    }
    it->release();
    return found;
}

static void nvdaAgdcDropTable(void)
{
    IOService *old[kAgdcMaxFB] = {};
    unsigned n = 0;
    if (gAgdcLock) IOLockLock(gAgdcLock);
    n = gAgdcNFB;
    gAgdcNFB = 0;
    for (unsigned i = 0; i < kAgdcMaxFB; i++) { old[i] = gAgdcFB[i]; gAgdcFB[i] = nullptr; }
    if (n) snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "released (the last table had %u slot(s))", n);
    if (gAgdcLock) IOLockUnlock(gAgdcLock);
    for (unsigned i = 0; i < kAgdcMaxFB; i++) OSSafeReleaseNULL(old[i]);
}
static void nvdaAgdcDropFB(void)
{
    if (gAgdcFBRef) { gAgdcFBRef->release(); gAgdcFBRef = nullptr; }
    if (gAgdcPciRef) { gAgdcPciRef->release(); gAgdcPciRef = nullptr; }
    nvdaAgdcDropTable();
}

static void nvdaAgdcTargets(IOService **pci, IOService **fb)
{
    *pci = nullptr; *fb = nullptr;
    if (gAgdcFBRef) { gAgdcFBRef->release(); gAgdcFBRef = nullptr; }
    if (gAgdcPciRef) { gAgdcPciRef->release(); gAgdcPciRef = nullptr; }
    OSDictionary *m = IOService::serviceMatching("NVRMNVDAFramebuffer");
    if (m) {
        IOService *f = IOService::copyMatchingService(m);
        m->release();
        if (f) {
            IOService *p = nvdaFindPciAncestor(f);
            if (p) {
                p->retain();
                gAgdcPciRef = p; gAgdcFBRef = f; *fb = f; *pci = p;
                return;
            }
            f->release();
        }
    }
    IOService *p = nullptr;
    IOService *accel = nvdaFindNvAccelOnPci(nullptr, &p);
    if (!accel || !p) { OSSafeReleaseNULL(accel); return; }
    uint64_t head4RegId = 0;
    (void)nvdaReadU64Prop(accel, "NVAccelHead4RegistryID", &head4RegId);
    IOService *winner = nullptr;
    const char *classes[] = { "NMIntelFB", "NMGopFB" };
    for (unsigned ci = 0; ci < 2 && !winner; ci++) {
        OSDictionary *hm = IOService::serviceMatching(classes[ci]);
        OSIterator *it = hm ? IOService::getMatchingServices(hm) : nullptr;
        OSSafeReleaseNULL(hm);
        if (!it) continue;
        OSObject *o;
        while ((o = it->getNextObject())) {
            IOService *f = OSDynamicCast(IOService, o);
            if (!f || f->isInactive()) continue;
            const uint64_t rid = f->getRegistryEntryID();
            uint32_t tmp = 0;
            const int hasAccelIdx = nvdaReadU32Prop(f, "IOAccelIndex", &tmp) ? 1 : 0;
            const int hasHybridProp = (nvdaReadU32Prop(f, "NMIntelFBHybridLevel", &tmp) || ci == 1) ? 1 : 0;
            if (!nvrm_agdc_should_enroll_hybrid(1, head4RegId, rid, hasAccelIdx, hasHybridProp)) continue;
            f->retain();
            winner = f;
            break;
        }
        it->release();
    }
    if (winner) {
        p->retain();
        gAgdcPciRef = p; gAgdcFBRef = winner;
        *fb = winner; *pci = p;
        FBLOG("AGDC: hybrid target %s (%#llx) paired with %s (%s)",
              winner->getName(), (unsigned long long)winner->getRegistryEntryID(),
              p->getName(), head4RegId ? "head4" : "accel");
    }
    accel->release();
}

static int gAgdcScanLogged = 0;
#define SCANLOG(fmt, ...) do { if (gAgdcScanLogged < 8) { gAgdcScanLogged++; FBLOG("AGDC scan: " fmt " [%d/8]", ##__VA_ARGS__, gAgdcScanLogged); } } while (0)
static void nvdaAgdcScan(IOService *pci)
{
    gAgdcScanLogged = 0;
    IOService *accel = nvdaFindNvAccelOnPci(pci, nullptr);
    uint64_t head4RegId = 0; uint32_t head4Dense = 0;
    int hasHead4Dense = 0;
    if (accel) {
        (void)nvdaReadU64Prop(accel, "NVAccelHead4RegistryID", &head4RegId);
        hasHead4Dense = nvdaReadU32Prop(accel, "NVAccelHead4DensePipeIndex", &head4Dense) ? 1 : 0;
    }

    struct nvrm_agdc_candidate cands[NVRM_AGDC_MAX_CANDIDATES];
    bzero(cands, sizeof cands);
    IOService *refs[NVRM_AGDC_MAX_CANDIDATES] = {};
    unsigned ncands = 0, seen = 0, other = 0;

    const char *classes[3] = { "NVRMNVDAFramebuffer", "NMIntelFB", "NMGopFB" };
    const unsigned nclasses = accel ? 3u : 1u;
    for (unsigned ci = 0; ci < nclasses && seen < NVRM_AGDC_MAX_CANDIDATES; ci++) {
        OSDictionary *m = IOService::serviceMatching(classes[ci]);
        OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
        OSSafeReleaseNULL(m);
        if (!it) continue;
        OSObject *o;
        while (seen < NVRM_AGDC_MAX_CANDIDATES && (o = it->getNextObject())) {
            seen++;
            IOService *f = OSDynamicCast(IOService, o);
            if (!f || f->isInactive()) continue;
            const uint64_t rid = f->getRegistryEntryID();
            uint32_t accelIdx = 0, depIdx = 0, hyLv = 0;
            const int hasAccelIdx = nvdaReadU32Prop(f, "IOAccelIndex", &accelIdx) ? 1 : 0;
            const int hasDepIdx = nvdaReadU32Prop(f, "IOFBDependentIndex", &depIdx) ? 1 : 0;
            if (ci == 0) {
                if (nvdaFindPciAncestor(f) != pci) {
                    other++;
                    SCANLOG("framebuffer id %#llx is under another PCI device: not ours to report", (unsigned long long)rid);
                    continue;
                }
                cands[ncands].kind = NVRM_AGDC_FB_NATIVE;
            } else {
                const int hasHybridProp = (nvdaReadU32Prop(f, "NMIntelFBHybridLevel", &hyLv) || ci == 2) ? 1 : 0;
                if (!nvrm_agdc_should_enroll_hybrid(1, head4RegId, rid, hasAccelIdx, hasHybridProp)) {
                    other++;
                    SCANLOG("hybrid framebuffer id %#llx does not match PCI %s: left out", (unsigned long long)rid, pci ? pci->getName() : "(null)");
                    continue;
                }
                cands[ncands].kind = NVRM_AGDC_FB_HYBRID;
                cands[ncands].matches_head4 = (head4RegId && rid == head4RegId) ? 1 : 0;
                cands[ncands].has_head4_dense = (cands[ncands].matches_head4 && hasHead4Dense) ? 1 : 0;
                cands[ncands].head4_dense = head4Dense;
            }
            cands[ncands].registry_id = rid;
            cands[ncands].has_accel_index = hasAccelIdx;
            cands[ncands].accel_index = accelIdx;
            cands[ncands].has_dep_index = hasDepIdx;
            cands[ncands].dep_index = depIdx;
            f->retain();
            refs[ncands] = f;
            ncands++;
        }
        it->release();
    }
    OSSafeReleaseNULL(accel);

    struct nvrm_agdc_plan plan;
    nvrm_agdc_plan_slots(cands, ncands, kAgdcMaxFB, &plan);
    IOService *tab[kAgdcMaxFB] = {};
    for (unsigned i = 0; i < plan.count; i++) {
        const int ci = plan.slot_candidate[i];
        tab[i] = refs[ci];
        tab[i]->retain();
    }
    for (unsigned i = 0; i < ncands; i++) OSSafeReleaseNULL(refs[i]);

    char mapBuf[kAgdcFBMapBytes];
    int w = snprintf(mapBuf, sizeof mapBuf, "n=%u", plan.count);
    for (unsigned i = 0; i < plan.count && w > 0 && (unsigned)w < sizeof mapBuf; i++)
        w += snprintf(mapBuf + w, sizeof mapBuf - (size_t)w, " [%u]id %#llx", i, (unsigned long long)tab[i]->getRegistryEntryID());
    if (w > 0 && (unsigned)w < sizeof mapBuf &&
        (plan.left_out_noidx | plan.left_out_range | plan.left_out_dup | other | plan.left_out_hole)) {
        snprintf(mapBuf + w, sizeof mapBuf - (size_t)w,
                 " (left out: noidx %u range %u dup %u gpu %u hole %u)",
                 plan.left_out_noidx, plan.left_out_range, plan.left_out_dup, other, plan.left_out_hole);
    }

    IOService *old[kAgdcMaxFB] = {};
    if (gAgdcLock) IOLockLock(gAgdcLock);
    for (unsigned i = 0; i < kAgdcMaxFB; i++) {
        old[i] = gAgdcFB[i];
        gAgdcFB[i] = (i < plan.count) ? tab[i] : nullptr;
    }
    gAgdcNFB = plan.count;
    strlcpy(gAgdcFBMap, mapBuf, sizeof gAgdcFBMap);
    if (gAgdcLock) IOLockUnlock(gAgdcLock);
    for (unsigned i = 0; i < kAgdcMaxFB; i++) OSSafeReleaseNULL(old[i]);
    FBLOG("AGDC: slot table %s (%u framebuffer node(s) seen)", mapBuf, seen);
}

static SInt32 gAgdcBusy = 0;
static void nvdaAgdcWorkBody(void);
static void nvdaAgdcWork(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gAgdcBusy); nvdaAgdcWorkBody(); OSDecrementAtomic(&gAgdcBusy);
}
static void nvdaAgdcWorkBody(void)
{
    if (gAgdcWant == 1 && !gAgdc) {
        IOService *pci = nullptr, *fb = nullptr; nvdaAgdcTargets(&pci, &fb);
        if (!pci || !fb) { gAgdcState = -1; FBLOG("AGDC: no PCI device or no framebuffer yet (pci %p fb %p)", pci, fb); return; }
        nvdaAgdcScan(pci);
        NVDADeviceControl *d = OSTypeAlloc(NVDADeviceControl);
        if (!d) { gAgdcState = -2; FBLOG("AGDC: alloc failed"); nvdaAgdcDropFB(); return; }
        if (!d->init(nullptr)) { gAgdcState = -3; FBLOG("AGDC: init failed"); d->release(); nvdaAgdcDropFB(); return; }
        d->fPci = pci; d->fFB = fb;
        if (!d->attach(pci)) { gAgdcState = -4; FBLOG("AGDC: attach(%s) failed", pci->getName()); d->release(); nvdaAgdcDropFB(); return; }
        if (!d->start(pci)) { gAgdcState = -5; FBLOG("AGDC: AppleGraphicsDeviceControl::start failed (vendor calls so far %lld)", gAgdcCmds); d->detach(pci); d->release(); nvdaAgdcDropFB(); return; }
        gAgdc = d; gAgdcState = 1;
        FBLOG("AGDC: UP on %s — %u framebuffer slot(s), maxFramebuffers %d; vendor calls %lld, refused %lld (last %#x)", pci->getName(), gAgdcNFB, gAgdcMaxFBOut, gAgdcCmds, gAgdcUnsupported, gAgdcLastRefused);
    } else if (gAgdcWant == 0 && gAgdc) {
        NVDADeviceControl *d = gAgdc; gAgdc = nullptr;
        bool ok = d->terminate(kIOServiceRequired | kIOServiceSynchronous); d->release(); nvdaAgdcDropFB(); gAgdcState = 2;
        FBLOG("AGDC: terminate -> %d (a reboot is the certain way back; this is the polite one)", ok);
    }
}
static int nvrmfb_agdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAgdcState; int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    gAgdcWant = v;
    if (!gAgdcCall) gAgdcCall = thread_call_allocate(nvdaAgdcWork, nullptr);
    if (!gAgdcCall) return ENOMEM;
    thread_call_enter(gAgdcCall); return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvrmfb_agdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvrmfb_agdc_sysctl, "I",
            "write 1 = create our AppleGraphicsDeviceControl object, 0 = terminate it; reads the state (1 UP, <0 failed step)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_cmds,    CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcCmds,        "AGDC vendor calls received");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcUnsupported, "AGDC vendor calls answered Unsupported");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_last_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcLastRefused, 0, "last AGDC command refused");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5,      CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcK5,     0, "K5: 1 = answer kAGDCLinkConfig + plane-scaler capabilities (0 after every boot)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_link, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Link,    "K5: kAGDCLinkConfig queries answered");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_caps, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Caps,    "K5: plane-scaler capability queries answered");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_why,  CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Why,  0, "K5: why the last K5 query was not answered");
SYSCTL_STRING(_debug, OID_AUTO, nvrmfb_agdc_fbmap, CTLFLAG_RD | CTLFLAG_LOCKED, gAgdcFBMap, sizeof gAgdcFBMap, "the slot table our 0x980 reports: n, then [i]id per IOFBDependentIndex");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_rescans, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcRescans, "slot-table rescans done while answering 0x980 (a framebuffer that started after boot)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_maxfb,   CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcMaxFBOut, 0, "maxFramebuffers in our last 0x980 reply (0 = never asked)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_asked, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Asked, 0, "bit i = a 0x921/0x711 named index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_link_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LinkMask, 0, "bit i = kAGDCLinkConfig answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_caps_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5CapsMask, 0, "bit i = the plane-scaler capability answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_last_refused_idx, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LastRefusedIdx, 0, "the index the last BADARG/NOTFOUND refusal named (-1 none); an Unsupported 0x711 type shows in k5_asked only");
static void nvdaAgdcRemoveSysctl(void)
{
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_rescans);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
}
static void nvdaAgdcInstallSysctl(void)
{
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_rescans);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
}

class NVDAGDCLoader : public IOService {
    OSDeclareDefaultStructors(NVDAGDCLoader)
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVDAGDCLoader, IOService)
bool NVDAGDCLoader::start(IOService *provider)
{
    { if (nvrm_driver_off()) return false; }
    if (!IOService::start(provider)) return false;
    if (!gAgdcLock) {
        gAgdcLock = IOLockAlloc();
        if (!gAgdcLock) { IOService::stop(provider); return false; }
    }
    nvdaAgdcInstallSysctl(); FBLOG("loaded; idle until `sysctl debug.nvrmfb_agdc=1`"); return true;
}
void NVDAGDCLoader::stop(IOService *provider)
{
    nvdaAgdcRemoveSysctl();
    if (gAgdcCall) {
        thread_call_cancel(gAgdcCall); int spins = 0;
        while (gAgdcBusy && spins++ < 200) IOSleep(10);
        if (gAgdcBusy) FBLOG("stop: the worker is STILL running after 2 s — leaving its thread call allocated rather than freeing it under it");
        else { thread_call_free(gAgdcCall); gAgdcCall = nullptr; }
    }
    if (gAgdc && !gAgdcBusy) { gAgdcWant = 0; nvdaAgdcWorkBody(); }
    if (!gAgdcBusy && gAgdcLock) { IOLockFree(gAgdcLock); gAgdcLock = nullptr; }
    IOService::stop(provider);
}
