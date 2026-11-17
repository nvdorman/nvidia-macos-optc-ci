#!/usr/bin/env python3
"""Exercise production firmware authority, shadow mapping and console lifecycle.

Only external IOKit/MMIO effects are fixtures. The policy header and extracted
NMIntelShadowMemory/refresh/close/restore methods are compiled without rewriting.
This is host behavioral evidence, not a loaded-kernel or panel test.
"""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "kexts/NMIntelFB/intelfb.cpp").read_text()


def definition(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth = 1
    i = opening + 1
    # These selected definitions contain no string/comment brace characters.
    while depth:
        if SOURCE[i] == "{":
            depth += 1
        elif SOURCE[i] == "}":
            depth -= 1
        i += 1
    return SOURCE[start:i] + "\n"


MAPPING = definition("class NMIntelShadowMemory final") + ";\n"
METHODS = "\n".join(definition(name) for name in (
    "static NMIntelVideo nmIntelConsoleVideo(",
    "bool NMIntelFB::recordConsoleMappingLocked(",
    "void NMIntelFB::revokePresentation(",
    "IOReturn NMIntelFB::reacquirePresentation(",
    "bool NMIntelFB::copyShadowLocked(",
    "void NMIntelFB::refreshFire(",
    "bool NMIntelFB::restoreConsole(",
    "IOReturn NMIntelFB::gatedCloseAction(",
    "IOReturn NMIntelFB::open(",
    "void NMIntelFB::close(",
    "IOReturn NMIntelFB::setupForCurrentConfig(",
    "IOReturn NMIntelFB::setDisplayMode(",
    "IODeviceMemory *NMIntelFB::getApertureRange(",
    "IOReturn NMIntelFB::framebufferInfo(",
))

PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "intelfb_console.h"
#include "nvrm-agdc-k5.h"

static uint64_t gFakeMachTimeNs = 1000000000ull;
static uint64_t mach_absolute_time(void) { return gFakeMachTimeNs; }

using IOOptionBits = uint32_t;
using IOByteCount = uint64_t;
using IOVirtualAddress = uintptr_t;
using IOReturn = int;
using IOPixelAperture = int;
using IOPhysicalLength = uint64_t;
using IODisplayModeID = uint32_t;
using IOIndex = int32_t;
using UInt32 = uint32_t;
using thread_call_param_t = void *;
using task_t = void *;
struct OSObject { virtual ~OSObject() = default; };
struct IOWorkLoop {
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    bool gateHeld = false;
    unsigned gatedActions = 0;
    IOReturn runAction(Action action, OSObject *target, void *p0 = nullptr, void *p1 = nullptr, void *p2 = nullptr, void *p3 = nullptr) {
        assert(!gateHeld);
        gateHeld = true;
        ++gatedActions;
        const IOReturn r = action(target, p0, p1, p2, p3);
        gateHeld = false;
        return r;
    }
};
constexpr unsigned kIOMapCacheMask = 0xf00, kIOMapDefaultCache = 0;
constexpr unsigned kIOMapWriteCombineCache = 0x400, kIOMapInhibitCache = 0x100;
constexpr unsigned kIODirectionInOut = 3;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNotReady = 1;
constexpr IOReturn kIOReturnBadArgument = 2, kIOReturnUnsupported = 3;
constexpr unsigned kPEBaseAddressChange = 8, kPEAcquireScreen = 4;
constexpr int kIOFBSystemAperture = 0, NMH_COPY = 1;
#define OSDeclareDefaultStructors(name)
#define IFBLOG(...) ((void)0)

struct IOLock { std::mutex mutex; std::atomic<bool> held{false}; };
static void IOLockLock(IOLock *lock) { lock->mutex.lock(); lock->held = true; }
static void IOLockUnlock(IOLock *lock) { lock->held = false; lock->mutex.unlock(); }
static bool IOLockTryLock(IOLock *lock) {
    if (!lock->mutex.try_lock()) return false;
    lock->held = true;
    return true;
}
static bool OSCompareAndSwap(UInt32 oldValue, UInt32 newValue, volatile UInt32 *target) {
    return __sync_bool_compare_and_swap(target, oldValue, newValue);
}

static unsigned destroyedBacking = 0, destroyedMaps = 0;
struct IOMemoryDescriptor {
    std::atomic<unsigned> references{1};
    uint64_t length = 0;
    explicit IOMemoryDescriptor(uint64_t n = 0) : length(n) {}
    virtual ~IOMemoryDescriptor() = default;
    void retain() { ++references; }
    void release() { if (--references == 0) delete this; }
    uint64_t getLength() const { return length; }
};
struct Backing : IOMemoryDescriptor {
    std::vector<uint32_t> pixels;
    explicit Backing(uint64_t n) : IOMemoryDescriptor(n), pixels((n + 3) / 4, 0) {}
    ~Backing() override { ++destroyedBacking; }
    void *getBytesNoCopy() { return pixels.data(); }
};
struct IOMemoryMap {
    IOMemoryDescriptor *descriptor = nullptr;
    unsigned options = 0;
    uintptr_t address = 0;
    uint64_t length = 0;
    ~IOMemoryMap() { if (descriptor) descriptor->release(); ++destroyedMaps; }
    void release() { delete this; }
    IOMemoryDescriptor *getMemoryDescriptor() { return descriptor; }
    uintptr_t getVirtualAddress() { return address; }
    unsigned getMapOptions() { return options; }
    uint64_t getLength() { return length; }
};
struct IOSubMemoryDescriptor : IOMemoryDescriptor {
    IOMemoryDescriptor *parent = nullptr;
    static bool failMap;
    ~IOSubMemoryDescriptor() override { if (parent) parent->release(); }
    bool initSubRange(IOMemoryDescriptor *p, uint64_t start, uint64_t n, unsigned direction) {
        assert(direction == kIODirectionInOut);
        if (!p || start > p->length || n > p->length - start) return false;
        p->retain(); parent = p; length = n; return true;
    }
    virtual IOMemoryMap *makeMapping(IOMemoryDescriptor *, task_t, IOVirtualAddress address,
                                     IOOptionBits options, IOByteCount offset, IOByteCount n) {
        auto *mapping = reinterpret_cast<IOMemoryMap *>(address);
        if (failMap || offset > length || n > length - offset) { mapping->release(); return nullptr; }
        mapping->descriptor = parent; parent->retain();
        mapping->address = reinterpret_cast<uintptr_t>(static_cast<Backing *>(parent)->getBytesNoCopy()) + offset;
        mapping->length = n;
        mapping->options = (options & ~kIOMapCacheMask) | kIOMapWriteCombineCache;
        return mapping;
    }
};
bool IOSubMemoryDescriptor::failMap = false;
struct IODeviceMemory { static IODeviceMemory *withRange(uint64_t, uint64_t) { assert(false); return nullptr; } };
'''

FIXTURE = r'''
struct PE_Video {
    unsigned long v_baseAddr = 0, v_rowBytes = 0, v_width = 0, v_height = 0;
    unsigned long v_depth = 32, v_display = 1;
    unsigned char v_rotate = 0;
};
struct { PE_Video video; } PE_state;
struct IOPlatformExpert {
    IOLock *lock = nullptr;
    unsigned changed = 0, acquired = 0;
    bool reject = false;
    IOReturn getConsoleInfo(PE_Video *out) {
        assert(!lock->held); *out = PE_state.video; return kIOReturnSuccess;
    }
    IOReturn setConsoleInfo(PE_Video *video, unsigned operation) {
        assert(!lock->held);
        if (reject) return kIOReturnNotReady;
        if (operation == kPEBaseAddressChange) { assert(video); PE_state.video = *video; ++changed; }
        else { assert(operation == kPEAcquireScreen && !video); ++acquired; }
        return kIOReturnSuccess;
    }
};
struct IOFramebuffer : OSObject {
    IOLock *fixtureLock = nullptr;
    IOWorkLoop *fixtureWorkLoop = nullptr;
    unsigned baseOpens = 0, baseCloses = 0, baseSetups = 0;
    IOReturn openStatus = kIOReturnSuccess, setupStatus = kIOReturnSuccess;
    void (*onOpen)() = nullptr;
    void (*onSetup)() = nullptr;
    IOWorkLoop *getWorkLoop() const { return fixtureWorkLoop; }
    IOReturn open() { ++baseOpens; if (onOpen) onOpen(); return openStatus; }
    void close() { assert(!fixtureLock->held); assert(!fixtureWorkLoop || fixtureWorkLoop->gateHeld); ++baseCloses; }
    IOReturn setupForCurrentConfig() { ++baseSetups; if (onSetup) onSetup(); return setupStatus; }
};
struct NMIntelFB : IOFramebuffer {
    IOLock lock;
    IOWorkLoop workLoop;
    IOLock *fPresentLock = &lock;
    IOPlatformExpert platform;
    bool fConsoleScanout = true, fOpened = true, fPowered = true, fStopping = false, fSetupInFlight = false;
    bool authority = true;
    int fHyLevel = NMH_COPY;
    uint32_t fWidth = 320, fHeight = 200, fRowBytes = 1280;
    uint64_t fFrameBytes = uint64_t(fRowBytes) * fHeight, fConsoleKva = 0;
    uint64_t fApertureBase = 0, fSurf = 0;
    NMIntelPresentState fPresentState{NMIntelApertureActive, 1, 0, 0};
    NMIntelVideo fBootVideo{0x94000000, fWidth, fHeight, fRowBytes, 32, 1, 0};
    PE_Video fSavedConsole;
    Backing *fShadowBuffer = nullptr;
    NMIntelShadowMemory *fShadowMemory = nullptr;
    IOMemoryMap *fSurfMap = nullptr, *fVramMap = nullptr;
    volatile unsigned char *fFrameBuffer = nullptr;
    uint64_t fVramMapOffset = 0;
    volatile UInt32 fRefreshPending = 1;
    uint8_t *fSinkMirror = nullptr;
    uint64_t fSinkMirrorEpoch = 0;
    uint64_t fRefreshMinAbs = 50000000ull;
    uint64_t fRefreshNextAbs = 0;
    unsigned copies = 0;
    NMIntelFB() { fixtureLock = &lock; fixtureWorkLoop = &workLoop; platform.lock = &lock; }
    IOPlatformExpert *getPlatform() { return &platform; }
    uint64_t getRegistryEntryID() { return 17; }
    bool revalidateAuthorityLocked(bool) { assert(lock.held); return authority && fPowered; }
    bool recordConsoleMappingLocked();
    void revokePresentation(NMIntelPhase);
    IOReturn reacquirePresentation();
    bool copyShadowLocked();
    static void refreshFire(thread_call_param_t, thread_call_param_t);
    bool restoreConsole();
    static IOReturn gatedCloseAction(OSObject *, void *, void *, void *, void *);
    IOReturn open();
    void close();
    IOReturn setupForCurrentConfig();
    IOReturn setDisplayMode(IODisplayModeID, IOIndex);
    IODeviceMemory *getApertureRange(IOPixelAperture);
    IOReturn framebufferInfo(NMIntelFramebufferInfo *);
};

static void casePass(const char *id) {
    std::printf("OPTC_CASE {\"id\":\"%s\",\"result\":\"PASS\"}\n", id);
}
static void write32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8)); }
static void write64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = uint8_t(v >> (i * 8)); }
static void efi(uint8_t *p, uint32_t type, uint64_t base, uint64_t pages, uint64_t attributes = 0) {
    write32(p, type); write64(p + 8, base); write64(p + 24, pages); write64(p + 32, attributes);
}
static void reservation() {
    NMIntelVideo video{0x94000000, 2560, 1600, 10240, 32, 1, 0};
    uint64_t bytes = 0, rounded = 0;
    assert(nm_intelfb_video_ok(&video, &bytes, &rounded));
    assert(bytes == 16384000 && rounded == bytes);
    assert(nm_intelfb_boot_header_ok(2, 0, 64) && nm_intelfb_boot_header_ok(2, 1, 64));
    assert(!nm_intelfb_boot_header_ok(1, 0, 64) && !nm_intelfb_boot_header_ok(2, 2, 64));
    assert(!nm_intelfb_boot_header_ok(2, 1, 32));
    assert(nm_intelfb_kernel_span(0xffffff8000100000ull, 4096));
    assert(!nm_intelfb_kernel_span(0x100000, 4096) && !nm_intelfb_kernel_span(UINT64_MAX - 7, 8));
    for (unsigned field = 0; field < 7; ++field) {
        NMIntelVideo bad = video;
        if (field == 0) bad.base |= 1;
        if (field == 1) bad.base = 0xffffff8000000000ull;
        if (field == 2) bad.pitch = UINT64_MAX - 3;
        if (field == 3) bad.rotate = 1;
        if (field == 4) bad.depth = 30;
        if (field == 5) bad.display = 0;
        if (field == 6) bad.display = 3;
        assert(!nm_intelfb_video_ok(&bad, nullptr, nullptr));
    }
    // Verbose (-v) boots leave bootArgs->Video.v_display == 2 (FB_TEXT_MODE) while XNU normalizes console to 1.
    NMIntelVideo verboseBoot = video;
    verboseBoot.display = 2;
    assert(nm_intelfb_video_ok(&verboseBoot, &bytes, &rounded));
    NMIntelVideo activeConsole = video;
    assert(nm_intelfb_console_matches(&verboseBoot, &activeConsole, 0));
    activeConsole.display = 2;
    assert(!nm_intelfb_console_matches(&verboseBoot, &activeConsole, 0));
    uint8_t map[96] = {};
    NMIntelEfiMap metadata{0x400000, sizeof map, 48, 1};
    efi(map, 0, video.base, 2000); efi(map + 48, 0, video.base + 2000 * 4096ull, 2000);
    assert(nm_intelfb_reserved(&metadata, map, video.base, rounded));
    uint8_t swap[48]; std::memcpy(swap, map, 48); std::memcpy(map, map + 48, 48); std::memcpy(map + 48, swap, 48);
    assert(nm_intelfb_reserved(&metadata, map, video.base, rounded));
    for (unsigned type = 1; type <= 14; ++type) {
        write32(map, type); assert(!nm_intelfb_reserved(&metadata, map, video.base, rounded));
    }
    write32(map, 0); write64(map + 32, 1ull << 63);
    assert(!nm_intelfb_reserved(&metadata, map, video.base, rounded)); write64(map + 32, 0);
    write64(map + 8, video.base + 1999 * 4096ull);
    assert(!nm_intelfb_reserved(&metadata, map, video.base, rounded)); // overlap
    write64(map + 8, video.base + 2001 * 4096ull);
    assert(!nm_intelfb_reserved(&metadata, map, video.base, rounded)); // gap
    efi(map, 0, video.base, rounded / 4096); metadata.bytes = 48;
    assert(nm_intelfb_reserved(&metadata, map, video.base, rounded));
    for (unsigned kind = 0; kind < 6; ++kind) {
        auto bad = metadata;
        if (kind == 0) --bad.bytes;
        if (kind == 1) bad.stride = 32;
        if (kind == 2) bad.version = 2;
        if (kind == 3) bad.bytes = NM_INTEL_EFI_MAP_MAX + 48;
        if (kind == 4) bad.address = UINT64_MAX - 32;
        if (kind == 5) bad.bytes = 0;
        assert(!nm_intelfb_reserved(&bad, map, video.base, rounded));
    }
    assert(!nm_intelfb_reserved(&metadata, map, video.base, rounded + 4096));
    write64(map + 24, UINT64_MAX); assert(!nm_intelfb_reserved(&metadata, map, video.base, rounded));
    casePass("reservation");
}

struct Registers {
    std::vector<uint32_t> words = std::vector<uint32_t>((16u << 20) / 4, 0);
    uint64_t pteReads = 0, mutateOffset = UINT64_MAX;
    unsigned mutateOccurrence = 0, seen = 0;
    void put(uint64_t at, uint32_t v) { words.at(at / 4) = v; }
    void put64(uint64_t at, uint64_t v) { put(at, uint32_t(v)); put(at + 4, uint32_t(v >> 32)); }
    static int read(void *ctx, uint64_t at, uint32_t *out) {
        auto &r = *static_cast<Registers *>(ctx);
        assert(at % 4 == 0 && at + 4 <= r.words.size() * 4); // production must bound every read
        if (at >= (8u << 20)) ++r.pteReads;
        *out = r.words[at / 4];
        if (at == r.mutateOffset && ++r.seen == r.mutateOccurrence) *out ^= 1;
        return 1;
    }
};
static void translation() {
    NMIntelVideo video{0x94000000, 2560, 1600, 10240, 32, 1, 0};
    Registers r;
    constexpr uint64_t table = 8u << 20, first = 0xff000;
    r.put(0x70008, 1u << 31); r.put(0x70180, (1u << 31) | (8u << 23));
    r.put(0x70188, 160); r.put(0x70190, (1599u << 16) | 2559);
    r.put(0x6001c, (2559u << 16) | 1599);
    r.put(0x7019c, 0xff000000); r.put(0x701ac, 0xff000000);
    r.put(0x108040, 0xc0); r.put64(0x108100, 0x90000000); r.put64(0x1080c0, 0x90800000);
    // General DPT controls remain zero. A linear plane still uses the GGTT.
    for (uint64_t i = 0; i < 4000; ++i) r.put64(table + (first + i) * 8, (0x4000000 + i * 4096) | 3);
    auto valid = [&] (uint64_t size = 16u << 20, unsigned version = 14) {
        return nm_intelfb_translation(Registers::read, &r, size, version, 0, &video, nullptr);
    };
    assert(valid() && r.pteReads == 4000 * 4);
    r.put64(table + first * 8, video.base | 1); assert(valid()); // system and local PTEs both resolve
    r.put64(table + first * 8, 0x4000003);
    r.put64(table + (first + 3999) * 8, (0x4000000 + 3998 * 4096) | 3); assert(!valid());
    r.put64(table + (first + 3999) * 8, (0x4000000 + 3999 * 4096) | 3);
    r.put64(table + first * 8, 0x4000002); assert(!valid()); r.put64(table + first * 8, 0x4000003);
    assert(!valid(8u << 20) && !valid(0x108043) && !valid(16u << 20, 13));
    r.put64(0x1080c0, 0x90900000); assert(!valid()); r.put64(0x1080c0, 0x90800000);
    r.put64(0x108100, 0xfffffffffffff000ull); assert(!valid()); r.put64(0x108100, 0x90000000);
    r.put(0x108040, 0x80); assert(!valid()); r.put(0x108040, 0xc0);
    r.put(0x701ac, 0xfe000000); assert(!valid()); r.put(0x701ac, 0xff000000);
    const uint32_t control = r.words[0x70180 / 4];
    for (uint32_t bit : {1u, 2u, 1u << 4, 1u << 8, 1u << 10, 1u << 15, 1u << 20}) {
        r.put(0x70180, control | bit); assert(!valid());
    }
    r.put(0x70180, control);
    for (uint32_t at : {0x7018cu, 0x701a4u}) { r.put(at, 1); assert(!valid()); r.put(at, 0); }
    r.mutateOffset = 0x70180; r.mutateOccurrence = 2; r.seen = 0; assert(!valid());
    r.mutateOffset = table + first * 8; r.mutateOccurrence = 2; r.seen = 0; assert(!valid());
    r.mutateOffset = UINT64_MAX; assert(valid());
    video.display = 2; assert(valid()); // verbose (-v) bootArgs FB_TEXT_MODE GGTT walk
    video.display = 1;
    casePass("translation");
}

static IOMemoryMap *mapShadow(NMIntelShadowMemory *shadow, unsigned options, uint64_t bytes) {
    return shadow->makeMapping(shadow, nullptr, reinterpret_cast<uintptr_t>(new IOMemoryMap), options, 0, bytes);
}
static void mapping() {
    const unsigned before = destroyedMaps, beforeBacking = destroyedBacking;
    auto *backing = new Backing(4096);
    auto *shadow = new NMIntelShadowMemory;
    assert(shadow->initSubRange(backing, 0, 4000, kIODirectionInOut));
    auto *defaultMap = mapShadow(shadow, 1, 4000);
    auto *wcMap = mapShadow(shadow, 0x401, 4000);
    assert(defaultMap && wcMap && defaultMap->getMapOptions() == 0x401 && wcMap->getMapOptions() == 0x401);
    for (unsigned cache : {0x100u, 0x200u, 0x300u, 0x500u, 0xf00u})
        assert(!mapShadow(shadow, cache | 1, 4000));
    assert(destroyedMaps == before + 5); // every refused provisional map consumed exactly once
    IOSubMemoryDescriptor::failMap = true; assert(!mapShadow(shadow, 0x401, 4000));
    IOSubMemoryDescriptor::failMap = false; assert(destroyedMaps == before + 6);
    casePass("cache_alias");
    shadow->release(); backing->release(); // framebuffer stopped; maps alone retain owned pages
    assert(destroyedBacking == beforeBacking);
    auto *client = reinterpret_cast<uint32_t *>(defaultMap->address);
    client[0] = 0x12345678; client[999] = 0xabcdef01;
    assert(reinterpret_cast<uint32_t *>(wcMap->address)[0] == 0x12345678);
    assert(reinterpret_cast<uint32_t *>(wcMap->address)[999] == 0xabcdef01);
    defaultMap->release(); assert(destroyedBacking == beforeBacking);
    wcMap->release(); assert(destroyedBacking == beforeBacking + 1 && destroyedMaps == before + 8);
    casePass("client_map_lifetime");
}

static void transfer() {
    constexpr unsigned width = 2560, height = 1600, pitch = 10240;
    constexpr uint64_t bytes = uint64_t(pitch) * height;
    std::vector<uint32_t> source(bytes / 4, 0xff123456), shadow(bytes / 4 + 2, 0xaabbccdd), sink(shadow);
    NMIntelPresentState state{NMIntelApertureActive, 7, 0, 0};
    NMIntelPresentRequest q{1, sizeof(q), 17, 7, 9, 3, 1, source.data(), bytes, width, height, pitch,
                           NM_INTEL_FB_FORMAT_BGRA, 0};
    assert(nm_intelfb_present_check(&state, 17, width, height, pitch, bytes, bytes, &q) == NMIntelPresentOK);
    assert(nm_intelfb_copy_final(source.data(), bytes, pitch, shadow.data() + 1, bytes, sink.data() + 1, bytes,
                                 width, height, pitch));
    nm_intelfb_present_complete(&state, &q);
    assert(q.completedSequence == 1 && state.sequence == 1 && shadow == sink);
    std::fill(source.begin(), source.end(), 0xff654321);
    assert(sink[1] == 0xff123456 && shadow[1] == 0xff123456); // no deferred source borrowing
    const auto before = sink;
    for (unsigned error = 0; error < 6; ++error) {
        uint64_t srcBytes = bytes, dstBytes = bytes, outBytes = bytes;
        unsigned srcPitch = pitch, dstPitch = pitch, w = width;
        if (error == 0) --srcBytes;
        if (error == 1) --dstBytes;
        if (error == 2) --outBytes;
        if (error == 3) srcPitch -= 4;
        if (error == 4) dstPitch -= 4;
        if (error == 5) w = UINT32_MAX;
        assert(!nm_intelfb_copy_final(source.data(), srcBytes, srcPitch, shadow.data() + 1, dstBytes,
                                     sink.data() + 1, outBytes, w, height, dstPitch));
        assert(sink == before && shadow == before);
    }
    assert(!nm_intelfb_copy_final(reinterpret_cast<void *>(UINTPTR_MAX - 3), bytes, pitch,
                                 shadow.data() + 1, bytes, sink.data() + 1, bytes, width, height, pitch));
    assert(shadow.front() == 0xaabbccdd && shadow.back() == 0xaabbccdd);
    assert(nm_intelfb_present_check(&state, 17, width, height, pitch, bytes, bytes, &q) == NMIntelPresentStale);
    ++q.sequence; q.sourceOwnerGeneration = 8;
    assert(nm_intelfb_present_check(&state, 17, width, height, pitch, bytes, bytes, &q) == NMIntelPresentStale);
    q.sourceOwnerGeneration = 10; q.sequence = 1;
    assert(nm_intelfb_present_check(&state, 17, width, height, pitch, bytes, bytes, &q) == NMIntelPresentOK);
    q.format = 0; assert(nm_intelfb_present_check(&state, 17, width, height, pitch, bytes, bytes, &q) == NMIntelPresentUnsupported);
    q.format = NM_INTEL_FB_FORMAT_BGRA; q.sourceAllocationCookie = 0;
    assert(nm_intelfb_present_check(&state, 17, width, height, pitch, bytes, bytes, &q) == NMIntelPresentBadArgument);
    q.sourceAllocationCookie = 3; q.epoch = 6;
    assert(nm_intelfb_present_check(&state, 17, width, height, pitch, bytes, bytes, &q) == NMIntelPresentStale);
    q.epoch = 7; std::fill(source.begin(), source.end(), 0);
    assert(nm_intelfb_copy_final(source.data(), bytes, pitch, shadow.data() + 1, bytes, sink.data() + 1, bytes,
                                 width, height, pitch));
    nm_intelfb_present_complete(&state, &q); assert(q.completedSequence == 1 && sink[1] == 0);
    uint64_t full, visible;
    assert(!nm_intelfb_span(1, 2, UINT64_MAX - 3, UINT64_MAX, &full, &visible));
    assert(!nm_intelfb_span(4, 2, 32, 48, &full, &visible)); // visible fits but full pitch*height does not
    assert(nm_intelfb_span(4, 2, 32, 64, &full, &visible) && full == 64 && visible == 48);
}

struct Gate {
    std::mutex mutex; std::condition_variable condition; unsigned phase = 0;
    void signal(unsigned value) { std::lock_guard<std::mutex> guard(mutex); phase = value; condition.notify_all(); }
    void await(unsigned value) { std::unique_lock<std::mutex> guard(mutex); condition.wait(guard, [&] { return phase >= value; }); }
};
static void lifecycle() {
    NMIntelFB fb;
    fb.fShadowBuffer = new Backing(fb.fFrameBytes);
    fb.fShadowMemory = new NMIntelShadowMemory;
    assert(fb.fShadowMemory->initSubRange(fb.fShadowBuffer, 0, fb.fFrameBytes, kIODirectionInOut));
    fb.fVramMap = mapShadow(fb.fShadowMemory, 0x401, fb.fFrameBytes);
    fb.fFrameBuffer = reinterpret_cast<volatile unsigned char *>(fb.fVramMap->address);
    Backing *physical = new Backing(fb.fFrameBytes);
    fb.fSurfMap = new IOMemoryMap; fb.fSurfMap->descriptor = physical;
    fb.fSurfMap->address = reinterpret_cast<uintptr_t>(physical->getBytesNoCopy());
    fb.fSurfMap->length = fb.fFrameBytes; fb.fSurfMap->options = kIOMapInhibitCache;
    IOLockLock(&fb.lock); assert(fb.recordConsoleMappingLocked()); IOLockUnlock(&fb.lock);
    fb.fSavedConsole = {fb.fBootVideo.base | 1, fb.fRowBytes, fb.fWidth, fb.fHeight, 32, 1, 0};
    PE_state.video = {fb.fConsoleKva, fb.fRowBytes, fb.fWidth, fb.fHeight, 32, 1, 0};
    NMIntelVideo current = fb.fBootVideo;
    current.base |= 1; assert(nm_intelfb_console_matches(&fb.fBootVideo, &current, 0));
    current.base |= 2; assert(!nm_intelfb_console_matches(&fb.fBootVideo, &current, 0));
    current.base = fb.fConsoleKva; assert(nm_intelfb_console_matches(&fb.fBootVideo, &current, fb.fConsoleKva));
    current.base += 4096; assert(!nm_intelfb_console_matches(&fb.fBootVideo, &current, fb.fConsoleKva));
    auto *aperture = reinterpret_cast<NMIntelShadowMemory *>(fb.getApertureRange(0));
    assert(aperture == fb.fShadowMemory); aperture->release(); assert(!fb.getApertureRange(1));
    NMIntelFramebufferInfo info{1, sizeof(info), 0, 0, 0, 0, 0, 0, 0};
    assert(fb.framebufferInfo(&info) == kIOReturnSuccess && info.actualSize == fb.fFrameBytes && info.epoch == 1);
    std::fill(fb.fShadowBuffer->pixels.begin(), fb.fShadowBuffer->pixels.end(), 0xffaabbcc);
    NMIntelFB::refreshFire(&fb, nullptr);
    assert(physical->pixels[0] == 0xffaabbcc && fb.fPresentState.sequence == 0 && fb.fRefreshPending == 0);
    fb.fRefreshPending = 1;
    IOLockLock(&fb.lock);
    std::thread missed([&] { NMIntelFB::refreshFire(&fb, nullptr); }); missed.join();
    IOLockUnlock(&fb.lock); assert(fb.fRefreshPending == 0); // try-lock skips instead of waiting
    fb.workLoop.runAction(NMIntelFB::gatedCloseAction, &fb);
    assert(fb.baseCloses == 1 && fb.fPresentState.phase == NMIntelRecovery && fb.fPresentState.epoch == 2);
    assert(PE_state.video.v_baseAddr == (fb.fBootVideo.base | 1) && fb.platform.changed == 1 && fb.platform.acquired == 1);
    assert(fb.framebufferInfo(&info) == kIOReturnNotReady && info.actualSize == 0);
    fb.fRefreshPending = 1; fb.fShadowBuffer->pixels[0] = 0xfeedface;
    NMIntelFB::refreshFire(&fb, nullptr); assert(physical->pixels[0] == 0xffaabbcc); // recovery never overwrites PE
    PE_state.video.v_baseAddr = 0xffffff8000200000ull;
    assert(fb.restoreConsole() && fb.platform.changed == 1); // foreign takeover not replaced
    PE_state.video.v_baseAddr = fb.fConsoleKva; fb.platform.reject = true;
    assert(!fb.restoreConsole() && PE_state.video.v_baseAddr == fb.fConsoleKva);
    fb.platform.reject = false; assert(fb.restoreConsole());
    assert(nm_intelfb_activate(&fb.fPresentState));
    fb.authority = false; fb.fRefreshPending = 1; NMIntelFB::refreshFire(&fb, nullptr);
    assert(fb.fPresentState.phase == NMIntelRecovery && fb.fPresentState.epoch == 3);
    fb.authority = true; assert(nm_intelfb_activate(&fb.fPresentState));
    Gate gate;
    std::thread transfer([&] { IOLockLock(&fb.lock); gate.signal(1); gate.await(2);
        assert(fb.fPresentState.phase == NMIntelApertureActive); IOLockUnlock(&fb.lock); });
    gate.await(1);
    std::thread stop([&] { gate.signal(2); fb.revokePresentation(NMIntelStopped); });
    transfer.join(); stop.join();
    assert(fb.fPresentState.phase == NMIntelStopped && fb.fPresentState.epoch == 4);
    assert(!nm_intelfb_activate(&fb.fPresentState));
    fb.fStopping = true; assert(!fb.getApertureRange(0));
    NMIntelPresentState exhausted{NMIntelApertureActive, UINT64_MAX, 4, 5};
    assert(!nm_intelfb_revoke(&exhausted, NMIntelRecovery) && exhausted.phase == NMIntelStopped);
    fb.fShadowMemory->release(); fb.fShadowBuffer->release();
    assert(reinterpret_cast<uint32_t *>(fb.fVramMap->address)[0] == 0xfeedface);
    fb.fVramMap->release(); fb.fSurfMap->release();
    casePass("console_epoch");
}
static NMIntelFB *gHookFb = nullptr;
static void openRevokesAuthority() { gHookFb->authority = false; }
static void setupMapsAndNotifies() {
    NMIntelFB &fb = *gHookFb;
    if (fb.fVramMap) fb.fVramMap->release();
    fb.fVramMap = mapShadow(fb.fShadowMemory, 0x401, fb.fFrameBytes);
    fb.fFrameBuffer = reinterpret_cast<volatile unsigned char *>(fb.fVramMap->address);
    PE_state.video = {fb.fBootVideo.base, fb.fRowBytes, fb.fWidth, fb.fHeight, 32, 1, 0};
    // Simulate IOFramebuffer::doSetup(true) firing deliverDisplayModeDidChangeNotification before setup returns:
    // group 5 (IOAccelDisplayPipe) queries framebufferInfo() prior to Legacy group 0.
    NMIntelFramebufferInfo info{1, sizeof(info), 0, 0, 0, 0, 0, 0, 0};
    assert(fb.framebufferInfo(&info) == kIOReturnSuccess);
    assert(info.epoch == fb.fPresentState.epoch && fb.fPresentState.phase == NMIntelApertureActive);
}
static void openAndModeset() {
    NMIntelFB fb;
    gHookFb = &fb;
    fb.fOpened = false;
    fb.fPresentState = {NMIntelPrepared, 1, 0, 0};
    fb.fShadowBuffer = new Backing(fb.fFrameBytes);
    fb.fShadowMemory = new NMIntelShadowMemory;
    assert(fb.fShadowMemory->initSubRange(fb.fShadowBuffer, 0, fb.fFrameBytes, kIODirectionInOut));
    Backing *physical = new Backing(fb.fFrameBytes);
    fb.fSurfMap = new IOMemoryMap; fb.fSurfMap->descriptor = physical;
    fb.fSurfMap->address = reinterpret_cast<uintptr_t>(physical->getBytesNoCopy());
    fb.fSurfMap->length = fb.fFrameBytes; fb.fSurfMap->options = kIOMapInhibitCache;
    fb.fSavedConsole = {fb.fBootVideo.base | 1, fb.fRowBytes, fb.fWidth, fb.fHeight, 32, 1, 0};
    PE_state.video = {fb.fBootVideo.base, fb.fRowBytes, fb.fWidth, fb.fHeight, 32, 1, 0};

    // Negative control: half-mapped fVramMap / fFrameBuffer is strictly rejected.
    fb.fFrameBuffer = reinterpret_cast<volatile unsigned char *>(0x1000);
    IOLockLock(&fb.lock); assert(!fb.recordConsoleMappingLocked()); IOLockUnlock(&fb.lock);
    fb.fFrameBuffer = nullptr;

    // Open succeeds when IOFramebuffer::open() leaves fVramMap & fFrameBuffer null (doSetup runs later).
    assert(fb.open() == kIOReturnSuccess && fb.baseOpens == 1 && fb.baseCloses == 0);
    assert(fb.fOpened && fb.fPresentState.phase == NMIntelApertureActive && fb.fConsoleKva == 0);

    // First setDisplayMode(1, 0) also runs before doSetup(1) maps fVramMap.
    assert(fb.setDisplayMode(1, 0) == kIOReturnSuccess);
    assert(fb.setDisplayMode(2, 0) == kIOReturnUnsupported);

    // setupForCurrentConfig activates synchronously once doSetup(1) remaps fVramMap so
    // IOAccelDisplayPipe::init_framebuffer_resource receives an active epoch during DidChange.
    const uint64_t preSetupEpoch = fb.fPresentState.epoch;
    fb.onSetup = setupMapsAndNotifies;
    assert(fb.setupForCurrentConfig() == kIOReturnSuccess && fb.baseSetups == 1);
    assert(fb.fPresentState.phase == NMIntelApertureActive);
    assert(fb.fPresentState.epoch == preSetupEpoch + 1);
    assert(!fb.fSetupInFlight && fb.fConsoleKva == reinterpret_cast<uintptr_t>(fb.fFrameBuffer));
    fb.onSetup = nullptr;

    // Outside setupForCurrentConfig(), revoked presentation stays NotReady even when fOpened & mapped.
    fb.revokePresentation(NMIntelRecovery);
    NMIntelFramebufferInfo info{1, sizeof(info), 0, 0, 0, 0, 0, 0, 0};
    assert(fb.framebufferInfo(&info) == kIOReturnNotReady && info.actualSize == 0);
    assert(fb.reacquirePresentation() == kIOReturnSuccess);

    // Open failure unwind after IOFramebuffer::open(): close() MUST run under controller workLoop gate.
    fb.fOpened = false;
    fb.fVramMap->release(); fb.fVramMap = nullptr; fb.fFrameBuffer = nullptr;
    fb.onOpen = openRevokesAuthority;
    assert(fb.open() == kIOReturnNotReady);
    assert(fb.workLoop.gatedActions == 1 && fb.baseCloses == 1 && !fb.fOpened && !fb.workLoop.gateHeld);
    fb.onOpen = nullptr;

    // Fallback unwind when controller workLoop is unavailable: no panic, state cleanly revoked.
    fb.authority = true;
    fb.fixtureWorkLoop = nullptr;
    fb.onOpen = openRevokesAuthority;
    assert(fb.open() == kIOReturnNotReady);
    assert(fb.baseCloses == 1 && !fb.fOpened && fb.fPresentState.phase == NMIntelRecovery);
    fb.fixtureWorkLoop = &fb.workLoop;

    fb.fShadowMemory->release(); fb.fShadowBuffer->release(); fb.fSurfMap->release();
    casePass("open_unwind_and_setup_sync");
}
static void diffMirrorAnd64Bit() {
    constexpr uint32_t W = 17, H = 4, P = W * 4;
    alignas(16) std::vector<uint32_t> src(W * H, 0x11223344u);
    alignas(16) std::vector<uint32_t> shadow(W * H, 0u);
    alignas(16) std::vector<uint32_t> sink(W * H, 0xdeadbeefu);
    alignas(16) std::vector<uint32_t> mirror(W * H, 0u);
    int mv = 0;
    // Also test 8-byte aligned even width + pitch first:
    constexpr uint32_t W8 = 16, P8 = W8 * 4;
    assert(nm_intelfb_copy_final_diff(src.data(), W8 * H * 4, P8,
        shadow.data(), W8 * H * 4, sink.data(), W8 * H * 4,
        mirror.data(), W8 * H * 4, &mv, W8, H, P8) == 1 && mv == 1);
    assert(sink[0] == 0x11223344u && sink[W8 * H - 1] == 0x11223344u);
    // Sentinel in sink proves identical pixels do NOT issue stores to UC- sink when mv == 1:
    std::fill(sink.begin(), sink.end(), 0xface0000u);
    assert(nm_intelfb_copy_final_diff(src.data(), W8 * H * 4, P8,
        shadow.data(), W8 * H * 4, sink.data(), W8 * H * 4,
        mirror.data(), W8 * H * 4, &mv, W8, H, P8) == 1 && mv == 1);
    assert(sink[0] == 0xface0000u && sink[5] == 0xface0000u);
    // Changing one pixel updates only its aligned 64-bit pair:
    src[5] = 0x55667788u;
    assert(nm_intelfb_copy_final_diff(src.data(), W8 * H * 4, P8,
        shadow.data(), W8 * H * 4, sink.data(), W8 * H * 4,
        mirror.data(), W8 * H * 4, &mv, W8, H, P8) == 1 && mv == 1);
    assert(sink[5] == 0x55667788u && sink[4] == 0x11223344u && sink[0] == 0xface0000u && sink[10] == 0xface0000u);
    // Epoch reset (mv = 0) resyncs all pixels cleanly:
    mv = 0;
    assert(nm_intelfb_copy_final_diff(src.data(), W8 * H * 4, P8,
        shadow.data(), W8 * H * 4, sink.data(), W8 * H * 4,
        mirror.data(), W8 * H * 4, &mv, W8, H, P8) == 1 && mv == 1);
    assert(sink[0] == 0x11223344u && sink[5] == 0x55667788u);
    // Odd width fallback (17 px, trailing pixel + 4-byte pitch):
    mv = 0;
    std::fill(sink.begin(), sink.end(), 0u);
    src[W * H - 1] = 0x99aabbccu;
    assert(nm_intelfb_copy_final_diff(src.data(), W * H * 4, P,
        shadow.data(), W * H * 4, sink.data(), W * H * 4,
        mirror.data(), W * H * 4, &mv, W, H, P) == 1 && mv == 1);
    assert(sink[W * H - 1] == 0x99aabbccu);
    // Overlap between mirror and shadow/sink/source is rejected safely:
    assert(nm_intelfb_copy_final_diff(src.data(), W8 * H * 4, P8,
        shadow.data(), W8 * H * 4, sink.data(), W8 * H * 4,
        sink.data(), W8 * H * 4, &mv, W8, H, P8) == 0);
    assert(nm_intelfb_copy_final_diff(src.data(), W8 * H * 4, P8,
        shadow.data(), W8 * H * 4, sink.data(), W8 * H * 4,
        mirror.data(), W8 * H * 4, nullptr, W8, H, P8) == 0);
    casePass("diff_mirror_and_64bit_transfer");
}
static void refreshCadenceAndLockFairness() {
    // 240 Hz VBL = 4.167 ms; minInterval = 50 ms must hold off console refreshes between ticks.
    assert(nm_intelfb_should_refresh(0, 1040000000ull, 1050000000ull) == 0);
    assert(nm_intelfb_should_refresh(0, 1050000000ull, 1050000000ull) == 1);
    // Once accelerated present() runs (sequence > 0), VBL NEVER queues console refreshFire.
    assert(nm_intelfb_should_refresh(1, 2000000000ull, 1050000000ull) == 0);
    // Adaptive backoff >= 4x copy duration and >= minInterval:
    assert(nm_intelfb_refresh_gap(1000000ull, 50000000ull) == 50000000ull);
    assert(nm_intelfb_refresh_gap(30000000ull, 50000000ull) == 120000000ull);

    NMIntelFB fb;
    fb.fShadowBuffer = new Backing(fb.fFrameBytes);
    std::vector<uint32_t> sink(fb.fWidth * fb.fHeight, 0u);
    std::vector<uint8_t> mirror(fb.fFrameBytes, 0u);
    fb.fSinkMirror = mirror.data();
    IOMemoryMap sinkMap;
    sinkMap.address = reinterpret_cast<uintptr_t>(sink.data());
    sinkMap.length = fb.fFrameBytes;
    fb.fSurfMap = &sinkMap;
    gFakeMachTimeNs = 2000000000ull;

    // With sequence == 0, refreshFire copies once and advances fRefreshNextAbs by >= 50 ms.
    fb.fRefreshPending = 1;
    NMIntelFB::refreshFire(&fb, nullptr);
    assert(fb.fRefreshPending == 0 && fb.fSinkMirrorEpoch == fb.fPresentState.epoch);
    assert(fb.fRefreshNextAbs >= gFakeMachTimeNs + fb.fRefreshMinAbs);

    // Once present() has activated (sequence > 0), refreshFire leaves console/sink untouched.
    fb.fPresentState.sequence = 42;
    static_cast<uint32_t *>(fb.fShadowBuffer->getBytesNoCopy())[0] = 0x12345678u;
    fb.fRefreshPending = 1;
    NMIntelFB::refreshFire(&fb, nullptr);
    assert(fb.fRefreshPending == 0 && sink[0] == 0u);

    fb.fSurfMap = nullptr;
    sinkMap.descriptor = nullptr;
    fb.fShadowBuffer->release();
    casePass("refresh_cadence_and_lock_fairness");
}
static void nvrmTimingK5() {
    uint32_t k5[NM_INTEL_NVRMTIMING_WORDS] = {};
    // Predator PHN16S-71 panel: 2560x1600 @ 240 Hz
    assert(nm_intelfb_build_nvrm_timing(2560, 1600, 2608, 2640, 2720, 1603, 1609, 1650,
                                        1077120000ull, 240000u, 1u, 1u, k5) == 1);
    assert(sizeof(k5) == 52 && nvk5_timing_bad(k5, sizeof k5) == 0);
    // Broken/zeroed firmware sync registers must sanitize cleanly and still satisfy AGDC K5.
    assert(nm_intelfb_build_nvrm_timing(2560, 1600, 0, 0, 0, 0, 0, 0, 0, 0, 1u, 1u, k5) == 1);
    assert(nvk5_timing_bad(k5, sizeof k5) == 0);
    // Reject zero raster.
    assert(nm_intelfb_build_nvrm_timing(0, 1600, 0, 0, 0, 0, 0, 0, 0, 0, 1u, 1u, k5) == 0);
    // Verify K5 consumer (nvk5_answer) accepts NMIntelFB timing for 0x921 and 0x711.
    assert(nm_intelfb_build_nvrm_timing(2560, 1600, 2608, 2640, 2720, 1603, 1609, 1650,
                                        1077120000ull, 240000u, 1u, 1u, k5) == 1);
    struct NVAGDCLinkConfig link{};
    int why = -1;
    assert(nvk5_answer(NVK5_CMD_LINKCONFIG, &link, sizeof link, &link, sizeof link, k5, sizeof k5, &why) == NVK5_R_LINK);
    assert(link.timing.hActive == 2560 && link.timing.vActive == 1600 && link.state == 1);
    struct NVAGDCFBExtCap cap{};
    cap.type = NVK5_CAP_PLANESCALER;
    assert(nvk5_answer(NVK5_CMD_FBCAPEX, &cap, sizeof cap, &cap, sizeof cap, k5, sizeof k5, &why) == NVK5_R_CAPS);
    assert(cap.count == 1);
    casePass("nvrm_timing_k5_contract");
}
int main() {
    reservation(); translation(); mapping(); transfer(); lifecycle(); openAndModeset();
    diffMirrorAnd64Bit(); refreshCadenceAndLockFairness(); nvrmTimingK5();
    puts("PASS: production console authority/mapping/transfer/lifecycle; host fixtures only");
}
'''

with tempfile.TemporaryDirectory(prefix="optc-console-") as directory:
    work = pathlib.Path(directory)
    source = work / "console.cpp"
    # The class must precede the fixture; production method bodies follow it.
    fixture_prefix, fixture_tests = FIXTURE.split("static void casePass", 1)
    source.write_text(PRELUDE + MAPPING + fixture_prefix + METHODS + "static void casePass" + fixture_tests)
    executable = work / "console"
    subprocess.run([
        os.environ.get("CXX", "clang++"), "-std=c++17", "-O1", "-g", "-pthread",
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(ROOT / "kexts/NMIntelFB"), "-I", str(ROOT / "kexts/NVRMAGDC"), str(source), "-o", str(executable),
    ], check=True)
    # Apple ASan rejects detect_leaks=1; retain each toolchain's default leak support.
    subprocess.run([str(executable)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "halt_on_error=1",
                        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
