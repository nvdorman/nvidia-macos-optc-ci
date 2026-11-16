#!/usr/bin/env python3
"""Run the production ownership transitions, retirement and native-unwind methods.

Only IOKit/RM effects are replaced. The methods below are extracted unchanged from
nvrm-accel.cpp; sanitizer failures and unbalanced native completes are fatal.
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "kexts/NVRM/accel/nvrm-accel.cpp").read_text()


def method(signature):
    start = SOURCE.index(signature)
    brace = SOURCE.index("{", start)
    depth, i, quote = 1, brace + 1, None
    while depth:
        c = SOURCE[i]
        if quote:
            if c == "\\":
                i += 2
                continue
            if c == quote:
                quote = None
        elif SOURCE.startswith("//", i):
            i = SOURCE.index("\n", i)
            continue
        elif SOURCE.startswith("/*", i):
            i = SOURCE.index("*/", i) + 2
            continue
        elif c in "\"'":
            quote = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return SOURCE[start:i] + "\n"


PRELUDE = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstring>
#include <vector>
#include "nvaccel_owner_policy.h"
#include "nvrm_gpuva_abi.h"
#include "nvrm_vram_abi.h"
#include "nm_intelfb_present_abi.h"
using IOReturn = int;
using IOOptionBits = unsigned;
using IOByteCount = unsigned long long;
using IOVirtualAddress = uintptr_t;
using task_t = void *;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnNotReady=1, kIOReturnNoMemory=2;
constexpr unsigned kIOMapCacheMask=0xf00, kIOMapCacheShift=8, kIOMapDefaultCache=0,
 kIOMapInhibitCache=0x100, kIOMapWriteCombineCache=0x400,
 kIOMemoryTypePhysical64=0x20, kIODirectionInOut=3, kIOMemoryMapperNone=0x800;
constexpr unsigned THREAD_UNINT=0, kMillisecondScale=1;
#define NVRM_OWNER_ABI_VERSION 1
#define NVRM_OWNER_CPU_ACCESS_FN "nvOwnerCpuAccess"
#define NVRM_GPUVA_DRAIN_FN "nvGpuVaDrain"
#define NVRM_OWNER_GENERATION_PROPERTY "NVVramOwnerGeneration"
struct IOLock { std::mutex m; std::condition_variable cv; };
static thread_local unsigned metadataDepth;
static void checkBackendCall();
static void drainFromBackend();
void IOLockLock(IOLock *l) { l->m.lock(); ++metadataDepth; }
void IOLockUnlock(IOLock *l) { assert(metadataDepth); --metadataDepth; l->m.unlock(); }
void IOLockWakeup(IOLock *l, void *, bool) { l->cv.notify_all(); }
void IOLockSleep(IOLock *l, void *, unsigned) {
 std::unique_lock<std::mutex> lock(l->m, std::adopt_lock); l->cv.wait(lock); lock.release();
}
struct Object {
 mutable unsigned refs=1;
 virtual ~Object() = default;
 void retain() const { ++refs; }
 void release() const { assert(refs); --refs; }
 virtual void free() {}
};
using OSObject=Object;
struct OSSymbol : Object { static OSSymbol *withCStringNoCopy(const char *) { static OSSymbol s; s.retain(); return &s; } };
struct IORegistryEntry : Object {
 IORegistryEntry *parent=nullptr;
 IORegistryEntry *getParentEntry(const char *) { return parent; }
};
static const char *gIOServicePlane="service";
struct OSDictionary : Object {};
struct OSIterator : Object {
 Object *child=nullptr; bool used=false;
 Object *getNextObject() { if(used)return nullptr;used=true;return child; }
};
struct OSNumber : Object { unsigned value=0x2d59; unsigned unsigned32BitValue(){return value;} };
struct IOService : IORegistryEntry {
 bool inactive=false; int callResult=0; Object *property=nullptr; OSIterator clients;
 static unsigned matchingCalls;
 bool init(){return true;} bool attach(IOService *p){parent=p;return true;} bool start(IOService *){return true;}
 bool isInactive() { return inactive; }
 void removeProperty(const char *) {}
 void stop(IOService *) {}
 void detach(IOService *) {}
 Object *getProperty(const char *) { return property; }
 OSIterator *getClientIterator(){clients.used=false;clients.retain();return &clients;}
 static OSDictionary *serviceMatching(const char *){++matchingCalls;static OSDictionary d;d.retain();return &d;}
 static OSIterator *getMatchingServices(OSDictionary *){static OSIterator it;it.used=false;it.retain();return &it;}
 bool didTerminate(IOService *, IOOptionBits, bool *) { return true; }
 IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) { checkBackendCall(); return callResult; }
};
unsigned IOService::matchingCalls=0;
#define OSTypeAlloc(T) new T
struct NvKmsKapiDevice {};
struct NvKmsKapiMemory {};
struct NvKmsKapiAllocateDeviceParams { unsigned gpuId; };
struct Kms {
 static Kms *instance;
 unsigned unmapped=0, freed=0, devices=0, allocated=0; NvKmsKapiDevice *device=nullptr;
 bool drainDuringAllocate=false;
 Kms(){instance=this;}
 NvKmsKapiDevice *(*allocateDevice)(NvKmsKapiAllocateDeviceParams *) = [](NvKmsKapiAllocateDeviceParams *){checkBackendCall();if(instance->drainDuringAllocate)drainFromBackend();++instance->allocated;return instance->device;};
 void (*unmapMemory)(NvKmsKapiDevice *,NvKmsKapiMemory *,int,void *) = [](NvKmsKapiDevice *,NvKmsKapiMemory *,int,void *){checkBackendCall();++instance->unmapped;};
 void (*freeMemory)(NvKmsKapiDevice *,NvKmsKapiMemory *) = [](NvKmsKapiDevice *,NvKmsKapiMemory *){checkBackendCall();++instance->freed;};
 void (*freeDevice)(NvKmsKapiDevice *) = [](NvKmsKapiDevice *){checkBackendCall();++instance->devices;};
 void *(*allocateMemory)() = []()->void *{return nullptr;};
 bool (*mapMemory)() = [](){return false;};
};
Kms *Kms::instance=nullptr;
using NvKmsKapiFunctionsTable=Kms;
using NvU32=unsigned;
constexpr int NVKMS_KAPI_MAPPING_TYPE_KERNEL=0;
struct thread_call { unsigned queued=0; };
static std::mutex deadlineLock;
static std::condition_variable deadlineCv;
static bool blockDeadline=false,deadlineEntered=false,releaseDeadline=false;
void clock_interval_to_deadline(unsigned n, unsigned, uint64_t *out) {
 std::unique_lock<std::mutex> lock(deadlineLock);
 if(blockDeadline){deadlineEntered=true;deadlineCv.notify_all();deadlineCv.wait(lock,[]{return releaseDeadline;});}
 *out=n;
}
void thread_call_enter_delayed(thread_call *call, uint64_t) { assert(metadataDepth==1);++call->queued; }
struct NVVramOwner;
struct NVVramAllocation;
struct NVGpuVaRecord;
struct NVStockLifetime : IOService { NVVramOwner *fOwner=nullptr; IOService *fFramebuffer=nullptr; bool fDeferred=false; IOOptionBits fOptions=0; };
struct NVRetiredScanout { IOService *framebuffer=nullptr; OSObject *frontSurface=nullptr,*previousSurface=nullptr; NVVramAllocation *front=nullptr,*previous=nullptr; };
struct NVRMOwnerCpuAccess {
 unsigned version, structSize; IOService *owner; uint64_t ownerGeneration;
 IOReturn (*body)(void *); void *context;
};
struct NVVramLease {
 NVVramAllocation *fAllocation=nullptr; NVLeaseUse fUse=NVLeaseUse::CpuAccess;
 ~NVVramLease(); void reset();
 NVVramAllocation *get() const { return fAllocation; }
 NVVramAllocation *take() { auto *a=fAllocation; fAllocation=nullptr; return a; }
};
struct NVVramOwner : IOService {
 IOLock storage, *fLock=&storage; NVOwnerState fState;
 IOService *fNvrm=nullptr,*fStock=nullptr; bool fDirect=true,fDeferred=false,fAttached=false,fLifetimeHeld=false,fRetirementPending=false;
 bool fInitializing=false,fReaping=false;
 unsigned fTerminateOptions=0; Kms *fKms=nullptr; NvKmsKapiDevice *fDevice=nullptr;
 NVStockLifetime *fStockLifetime=nullptr,*fDisplayLifetime[5]={};
 NVRetiredScanout fRetiredScanout[5];
 bool retireScanout(unsigned,IOService *,OSObject *,OSObject *,NVVramAllocation *,NVVramAllocation *);
 bool reapScanouts();
 NVRMRmHandles fHandles={}; NVVramAllocation *fAllocations=nullptr; NVGpuVaRecord *fReservations=nullptr;
 thread_call *fReaper=nullptr; uint64_t fMappedBytes=0;
 int failure=0; bool powered=true;
 bool active(); bool enter(); void leave(); void waitOperations(); bool borrow(); void unborrow();
 void beginDrain(); void reap(); void scheduleReap(unsigned delayMs=1);
 bool handles(NVRMRmHandles *); bool ensureDeviceLocked();
 IORegistryEntry *fPci=nullptr;
 IOReturn cpuAccess(IOReturn (*)(void *),void *);
 IOReturn call(const char *name,void *raw) {
  checkBackendCall();
  if (!std::strcmp(name,NVRM_OWNER_CPU_ACCESS_FN)) {
   auto *q=(NVRMOwnerCpuAccess *)raw;
   return powered && q->ownerGeneration==fState.generation ? q->body(q->context) : kIOReturnNotReady;
  }
  if (!std::strcmp(name,NVRM_GPUVA_DRAIN_FN)) return failure ? 1 : 0;
  if (!std::strcmp(name,NVRM_DISPLAY_PTRS_FN)) { ((NVRMDisplayPointers *)raw)->kapi=Kms::instance; return 0; }
  auto *q=(NVRMGpuVaRequest *)raw;
  if (failure==1) { q->status=1; return 1; }
  q->hMemory=0;
  if (failure==2) { q->status=2; return 2; }
  if (!(q->flags & NVRM_GPUVA_FLAG_UNMAP_ONLY)) { q->hVirt=0; q->gpuva=0; }
  q->status=0; return 0;
 }
};
static NVVramOwner *reentrantOwner;
static void checkBackendCall(){assert(metadataDepth==0);if(reentrantOwner)(void)reentrantOwner->active();}
static void drainFromBackend(){assert(reentrantOwner);reentrantOwner->beginDrain();}
struct NVVramAllocation : Object {
 NVVramOwner *fOwner=nullptr; NVVramAllocation *fNext=nullptr;
 NVBackingState fState; NVRMVramRequest fReceipt={}; unsigned fCache=kIOMapWriteCombineCache;
 uint64_t fChargedBytes=0;
 bool acquire(NVLeaseUse,NVVramLease *); void releaseLease(NVLeaseUse); void retire(); bool park(); bool unpark();
 NVBackingKey key() const { return fState.key; }
};
struct NVGpuVaRecord : Object {
 NVVramOwner *fOwner; NVGpuVaRecord *fNext=nullptr;
 NVRMGpuVaRequest fRequest={}; NVVramLease fLease; NVBackingKey fKey={}; bool fMapped=false,fRetiring=false,fBusy=false;
 bool unmap(bool);
};
struct IOAddressRange { uint64_t address,length; };
struct IOMemoryDescriptor : Object {};
struct IOMemoryMap : Object {};
struct IOGeneralMemoryDescriptor : IOMemoryDescriptor {
 unsigned creation=0; bool initWithOptions(void *,unsigned,unsigned,void *,unsigned o,void *) { creation=o; return true; }
 IOMemoryMap *makeMapping(IOMemoryDescriptor *,task_t,uintptr_t a,unsigned,IOByteCount,IOByteCount) { return (IOMemoryMap *)a; }
};
struct NVVramMemoryDescriptor : IOGeneralMemoryDescriptor {
 NVVramLease fLease;
 bool initWithAllocation(NVVramAllocation *,uint64_t,uint64_t);
 void free() override;
 IOMemoryMap *makeMapping(IOMemoryDescriptor *,task_t,IOVirtualAddress,IOOptionBits,IOByteCount,IOByteCount);
};
struct NVDisplayPipe;
struct IOAccelEvent {};
static NVVramOwner resourceOwner;
struct NVAccel { NVVramOwner *fOwner=&resourceOwner; bool fCandidate=true; bool familyOwned()const{return true;} };
static bool nvFamLock(NVAccel *,bool) { return true; }
static void nvFamUnlock(NVAccel *) {}
static NVAccel accelerator;
static unsigned gNVSVPageoffFail, legacyCopies;
#define ALOG(...) ((void)0)
struct IOSurface {
 uint64_t w=4,h=4,pitch=16; unsigned format=0x42475241;
 uint64_t getWidth(){return w;} uint64_t getHeight(){return h;} uint64_t getBytesPerRow(){return pitch;}
 unsigned getPixelFormat(){return format;}
};
struct IOAccelDisplayPipeTransaction2 {
 IOSurface *surface[2][2]={}; OSObject *resource[2][2]={};
 IOSurface *getPlaneIOSurface(unsigned p,unsigned i){return surface[p][i];}
 OSObject *getPlaneResource(unsigned p,unsigned i){return resource[p][i];}
};
constexpr IOReturn kIOReturnBadArgument=3,kIOReturnUnsupported=4;
#include "nv_iop_pixel.h"
static uint64_t nvResPitch(const unsigned char *) { return 16; }
static bool thread_call_cancel(thread_call *call) { if(call)call->queued=0;return true; }
struct NVVidMemory : Object {
 bool pin=true; unsigned prepares=0,completes=0; NVVramAllocation *fBacking=nullptr;
 bool prepare() { if(pin) ++prepares; return pin; }
 void complete() { assert(prepares>completes); ++completes; }
 uint64_t capacity() { return 16384000; }
};
struct IOAccelResource2 : Object {
 bool prepareOK=true; unsigned debt=0,completes=0;
 bool prepare() { if(prepareOK) ++debt; return prepareOK; }
 void complete() { assert(debt); --debt; ++completes; }
};
enum class NVFramebufferPhase { Empty,Candidate,Ready,Retiring };
struct NVResource : IOAccelResource2 {
 NVAccel *fAccel=&accelerator; bool gpuOnly=false;
 bool requirePageoff()const{return gpuOnly;}
 void pageoff(IOAccelEvent *,bool,bool *,unsigned long long);
 void pageoffLegacy(IOAccelEvent *,bool,bool *,unsigned long long){++legacyCopies;}
 NVDisplayPipe *fFramebufferPipe=nullptr; NVVidMemory *fFramebufferMemory=nullptr,*adopted=nullptr;
 unsigned fSuccessfulPrepares=0; bool fAbortWithoutPrepare=false,fMemoryPinned=false,descriptorOK=true;
 NVBackingKey fCpuPublicationKey={}; uint64_t fCpuPublication=0;
 bool prepare(); void complete(); void releaseFramebufferBacking();
 void removeFromAperture() {}
 void *getApertureMemoryDescriptor(void *) { return descriptorOK ? this : nullptr; }
};
struct IOAccelLegacyDisplayPipe : Object { void displayModeWillChange(); };
struct NVDisplayPipe : IOAccelLegacyDisplayPipe {
 NVAccel *fAccel=&accelerator;
 NVFramebufferPhase fFramebufferPhase=NVFramebufferPhase::Candidate; NVResource *fFramebufferResource=nullptr;
 NMIntelFramebufferInfo fFramebufferInfo={}; IOLock storage,*fTransactionLock=&storage; NVTransactionState fTransaction;
 void destroyFramebufferResource(unsigned,IOAccelResource2 *); void displayModeWillChange();
 IOReturn validateTransaction(IOAccelDisplayPipeTransaction2 *);
};
using IOAccelDisplayPipe=NVDisplayPipe;
#define OSDynamicCast(T,p) dynamic_cast<T *>(p)
NVVidMemory *nvres_vidmem(IOAccelResource2 *r) { return ((NVResource *)r)->adopted; }
void nvAccelDrainPipe(NVDisplayPipe *) {}
void IOAccelLegacyDisplayPipe::displayModeWillChange() {
 auto *p=(NVDisplayPipe *)this;
 while(p->fFramebufferResource && p->fFramebufferResource->fSuccessfulPrepares>1) p->fFramebufferResource->complete();
}
unsigned nativeDestroys=0;
void _ZN18IOAccelDisplayPipe28destroy_framebuffer_resourceEP16IOAccelResource2(IOAccelDisplayPipe *pipe,IOAccelResource2 *raw) {
 ++nativeDestroys; auto *r=(NVResource *)raw; r->complete();
 if(r->adopted) { r->adopted->release(); r->adopted=nullptr; }
 pipe->destroyFramebufferResource(0,r);
}
void pass(const char *id) { std::printf("OPTC_CASE {\"id\":\"%s\",\"result\":\"PASS\"}\n",id); }
'''
METHODS = [
    "bool NVVramOwner::active()", "bool NVVramOwner::enter()", "void NVVramOwner::leave()",
    "void NVVramOwner::waitOperations()", "bool NVVramOwner::borrow()", "void NVVramOwner::unborrow()",
    "bool NVVramOwner::handles(", "bool NVVramOwner::ensureDeviceLocked()", "IOReturn NVVramOwner::cpuAccess(",
    "void NVVramOwner::beginDrain()", "void NVVramOwner::reap()",
    "void NVVramOwner::scheduleReap(",
    "bool NVVramOwner::retireScanout(", "bool NVVramOwner::reapScanouts()",
    "NVVramLease::~NVVramLease()", "void NVVramLease::reset()",
    "bool NVVramAllocation::acquire(", "void NVVramAllocation::releaseLease(",
    "void NVVramAllocation::retire()", "bool NVVramAllocation::park()", "bool NVVramAllocation::unpark()",
    "bool NVGpuVaRecord::unmap(", "bool NVVramMemoryDescriptor::initWithAllocation(",
    "void NVVramMemoryDescriptor::free()", "IOMemoryMap *NVVramMemoryDescriptor::makeMapping(",
    "bool NVResource::prepare()", "void NVResource::complete()", "void NVResource::releaseFramebufferBacking()",
    "void NVDisplayPipe::destroyFramebufferResource(", "void NVDisplayPipe::displayModeWillChange()",
    "void NVResource::pageoff(", "IOReturn NVDisplayPipe::validateTransaction(",
]
CHECKS = r'''
int main() {
 assert(resourceOwner.fState.activate(100));
 uint64_t generation=0, first=0, second=0;
 assert(nvNextGeneration(&generation,&first)); assert(nvNextGeneration(&generation,&second));
 assert(first && second>first); generation=UINT64_MAX; assert(!nvNextGeneration(&generation,&second));
 assert(!nvBackingKeyEqual({first,1},{first,2})); assert(!nvBackingKeyEqual({first,1},{first+1,1}));
 NVVramOwner owner; Kms kms; NvKmsKapiDevice device; kms.device=&device;
 IOService nvrm,nub,pci; OSNumber gpuId; nub.property=&gpuId; nvrm.clients.child=&nub; owner.fNvrm=&nvrm; owner.fPci=&pci;
 assert(owner.fState.activate(first)); NVRMRmHandles initial={};
 reentrantOwner=&owner;
 assert(owner.handles(&initial) && initial.kapiDevice==&device && kms.allocated==1 && IOService::matchingCalls==0);
 NVVramAllocation allocation; allocation.fOwner=&owner; allocation.fState.key={first,1};
 allocation.fReceipt.actualSize=16384000; allocation.fReceipt.phys=0x10000000;
 std::vector<unsigned char> pixels(16384000);NvKmsKapiMemory memoryHandle;
 allocation.fReceipt.kva=pixels.data();allocation.fReceipt.handle=&memoryHandle;allocation.fChargedBytes=16384000;
 allocation.retain(); owner.fAllocations=&allocation; owner.fState.allocations=1; owner.fMappedBytes=16384000;
 NVVramLease lease; assert(allocation.acquire(NVLeaseUse::Present,&lease));
 assert(allocation.park() && allocation.unpark()); assert(nvBackingKeyEqual(allocation.key(),{first,1}));
 pass("owner_generation");
 IOService lateHead; owner.fStock=&lateHead; NVRMRmHandles h={};
 assert(owner.handles(&h) && h.kapiDevice==&device); assert(owner.fDirect);
 pass("late_head");
 unsigned bodyCalls=0;
 auto body=[](void *p)->IOReturn { ++*(unsigned *)p; return 0; };
 assert(owner.cpuAccess(body,&bodyCalls)==0 && bodyCalls==1 && !owner.fState.operations);
 owner.powered=false; assert(owner.cpuAccess(body,&bodyCalls)!=0 && bodyCalls==1); owner.powered=true;
 pass("power_lease");
 NVVramMemoryDescriptor descriptor; assert(descriptor.initWithAllocation(&allocation,0,16384000));
 assert(descriptor.creation==((4u<<28)|kIOMemoryTypePhysical64|kIODirectionInOut|kIOMemoryMapperNone));
 IOMemoryMap provisional;
 assert(descriptor.makeMapping(&descriptor,nullptr,(uintptr_t)&provisional,kIOMapInhibitCache,0,4)==nullptr);
 assert(provisional.refs==0);
 IOMemoryMap wc;
 assert(descriptor.makeMapping(&descriptor,nullptr,(uintptr_t)&wc,kIOMapDefaultCache,0,4)==&wc);
 owner.beginDrain(); assert(!owner.enter()); owner.reap(); assert(!kms.freed && !kms.devices);
 lease.reset(); owner.reap(); assert(!kms.freed); descriptor.free(); owner.reap();
 assert(kms.freed==1 && kms.devices==1 && owner.fState.phase==NVOwnerPhase::Dead);
 pass("client_map_lifetime");
 assert(owner.fMappedBytes==0);
 NVVramOwner initializing;initializing.fState.activate(first+20);initializing.fNvrm=&nvrm;initializing.fPci=&pci;
 reentrantOwner=&initializing;kms.drainDuringAllocate=true;const unsigned freedDevices=kms.devices;
 NVRMRmHandles refused={};assert(!initializing.handles(&refused)&&!refused.kapiDevice);
 assert(!initializing.fInitializing&&!initializing.fState.operations&&kms.devices==freedDevices+1);
 initializing.reap();assert(initializing.fState.phase==NVOwnerPhase::Dead);kms.drainDuringAllocate=false;
 reentrantOwner=&owner;
 pass("reentrant_callouts");
 NVVramOwner scheduled;scheduled.fState.activate(first+30);scheduled.fState.drain();thread_call work;scheduled.fReaper=&work;
 {std::lock_guard<std::mutex> lock(deadlineLock);blockDeadline=true;}
 std::thread enqueue([&]{scheduled.scheduleReap();});
 {std::unique_lock<std::mutex> lock(deadlineLock);deadlineCv.wait(lock,[]{return deadlineEntered;});}
 scheduled.reap();assert(scheduled.fState.phase==NVOwnerPhase::Dead);
 {std::lock_guard<std::mutex> lock(deadlineLock);releaseDeadline=true;deadlineCv.notify_all();}
 enqueue.join();assert(work.queued==0);
 {std::lock_guard<std::mutex> lock(deadlineLock);blockDeadline=false;}
 pass("reaper_shutdown");
 NVVramOwner concurrent; concurrent.fState.activate(first+1); concurrent.fKms=&kms; concurrent.fDevice=&device;
 std::mutex barrier; std::condition_variable cv; bool entered=false,release=false;
 std::thread user([&] { assert(concurrent.enter()); { std::unique_lock<std::mutex> l(barrier); entered=true; cv.notify_all(); cv.wait(l,[&]{return release;}); } concurrent.leave(); });
 { std::unique_lock<std::mutex> l(barrier); cv.wait(l,[&]{return entered;}); }
 concurrent.beginDrain(); assert(!concurrent.borrow()); concurrent.reap(); assert(concurrent.fState.phase==NVOwnerPhase::Draining);
 { std::lock_guard<std::mutex> l(barrier); release=true; cv.notify_all(); }
 concurrent.waitOperations(); user.join(); concurrent.reap(); assert(concurrent.fState.phase==NVOwnerPhase::Dead);
 pass("borrower_drain");
 NVVramOwner vaOwner; vaOwner.fState.activate(first+2);
 NVGpuVaRecord va; va.fOwner=&vaOwner; va.fMapped=true; va.fRequest.hVirt=1; va.fRequest.hMemory=2; va.fRequest.gpuva=0x1000;
 va.fBusy=true;assert(!va.unmap(true)&&va.fRequest.hVirt==1);va.fBusy=false;
 vaOwner.failure=1; assert(!va.unmap(true) && va.fMapped && va.fRequest.hMemory==2 && va.fRequest.hVirt==1);
 vaOwner.failure=2; assert(!va.unmap(true) && !va.fMapped && !va.fRequest.hMemory && va.fRequest.hVirt==1);
 vaOwner.failure=0; assert(va.unmap(true) && !va.fRequest.hVirt);
 pass("gpuva_retire");
 NVVramOwner displayOwner; displayOwner.fState.activate(44);
 NVVramAllocation front; front.fOwner=&displayOwner; NVVramLease displayLease;
 assert(front.acquire(NVLeaseUse::Present,&displayLease));
 IOService display; display.callResult=1;
 assert(displayOwner.retireScanout(0,&display,nullptr,nullptr,displayLease.take(),nullptr));
 assert(displayOwner.reapScanouts() && displayOwner.fState.borrowers==1 && !front.fState.idle());
 display.callResult=0; assert(!displayOwner.reapScanouts() && !displayOwner.fState.borrowers && front.fState.idle());
 pass("scanout_retire");
 for(unsigned failure=0;failure<3;++failure) {
  NVDisplayPipe pipe; NVResource r; NVVidMemory memory; NVVramAllocation a;
  memory.fBacking=&a; r.fFramebufferPipe=&pipe; pipe.fFramebufferResource=&r; r.adopted=&memory;
  if(failure==0) r.prepareOK=false;
  if(failure==1) memory.pin=false;
  if(failure==2) r.descriptorOK=false;
  unsigned before=nativeDestroys;
  assert(!r.prepare()); assert(nativeDestroys==before+1 && !r.adopted && !r.fFramebufferMemory && !r.fAbortWithoutPrepare);
  assert(r.completes==(failure==0 ? 0u:1u) && r.debt==0 && !pipe.fFramebufferResource);
 }
 pass("prepare_unwind");
 NVDisplayPipe pipe; NVResource r; NVVidMemory memory; NVVramAllocation backing; memory.fBacking=&backing;
 r.fFramebufferPipe=&pipe; r.adopted=&memory; pipe.fFramebufferResource=&r;
 assert(r.prepare()); assert(r.prepare()); assert(r.fSuccessfulPrepares==2);
 pipe.displayModeWillChange(); assert(!r.debt && r.completes==2 && memory.completes==1 && !pipe.fFramebufferResource);
 pass("mode_drain");
 NVVidMemory replacement; replacement.fBacking=&backing;
 r.fFramebufferPipe=&pipe; r.adopted=&replacement; pipe.fFramebufferResource=&r; pipe.fFramebufferPhase=NVFramebufferPhase::Candidate;
 assert(r.prepare()); pipe.displayModeWillChange(); assert(r.completes==3 && replacement.completes==1);
 pass("retry_balance");
 NVTransactionState tx; int one,two; tx.begin(&one,7,1,9);
 assert(!tx.finish(&two,1,9,{1,1},0,true)); assert(!tx.finish(&one,1,8,{1,1},0,true));
 assert(!tx.finish(&one,1,9,{1,1},1,true)); assert(!tx.matches(&one,7));
 assert(tx.finish(&one,1,9,{1,1},0,true)); assert(tx.matches(&one,7) && !tx.matches(&two,7) && !tx.matches(&one,8));
 tx.begin(&one,8,2,9); assert(!tx.finish(&one,2,9,{1,1},0,false));
 pass("transaction_completion");
 NVResource published; NVVidMemory allocated; published.adopted=&allocated;
 bool out=false; published.fCpuPublication=23;
 published.pageoff(nullptr,true,&out,0); assert(out && !legacyCopies && published.fCpuPublication==23);
 published.gpuOnly=true; out=true; published.pageoff(nullptr,true,&out,0);
 assert(!out && !legacyCopies && published.fCpuPublication==23 && !published.debt);
 pass("zero_candidate_ce");
 IOAccelDisplayPipeTransaction2 transaction; IOSurface image;
 transaction.surface[0][0]=&image;
 assert(pipe.validateTransaction(&transaction)==kIOReturnSuccess);
 for(unsigned p=0;p<2;++p)for(unsigned i=0;i<2;++i)if(p||i){
  transaction.surface[p][i]=&image;assert(pipe.validateTransaction(&transaction)==kIOReturnUnsupported);transaction.surface[p][i]=nullptr;
 }
 image.format=0;assert(pipe.validateTransaction(&transaction)==kIOReturnUnsupported);
 image.format=NV_IOP_FMT_BGRA;image.pitch=12;assert(pipe.validateTransaction(&transaction)==kIOReturnBadArgument);
 pass("multi_plane_reject");
}
'''

with tempfile.TemporaryDirectory(prefix="optc-lifetime-") as directory:
    root = pathlib.Path(directory)
    cpp = root / "lifetime.cpp"
    cpp.write_text(PRELUDE + "\n".join(method(s) for s in METHODS) + CHECKS)
    binary = root / "lifetime"
    subprocess.run([
        "clang++", "-std=c++17", "-pthread", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I" + str(ROOT / "kexts/NVRM/accel"),
        "-I" + str(ROOT / "kexts/NVRM"), str(cpp), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
