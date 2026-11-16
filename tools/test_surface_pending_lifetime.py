#!/usr/bin/env python3
"""Exercise production pending tables, timeout/refusal and resource destruction."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "kexts/NVRM/accel/nvrm-accel.cpp").read_text()


def method(signature):
    start = source.index(signature)
    pos = source.index("{", start)
    depth, i = 1, pos + 1
    while depth:
        if source.startswith("//", i):
            i = source.index("\n", i)
        elif source.startswith("/*", i):
            i = source.index("*/", i) + 2
        elif source[i] in "\"'":
            quote = source[i]
            i += 1
            while source[i] != quote:
                i += 2 if source[i] == "\\" else 1
            i += 1
        else:
            depth += (source[i] == "{") - (source[i] == "}")
            i += 1
    return source[start:i] + "\n"


table = source[source.index("enum { kNVSVPend ="):source.index("static void    *gNVSVRoot;")]
waits = source[source.index("static const unsigned kSurfGpuWaitMs"):source.index("// Unlocked page-off wait.")]
fixture = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <initializer_list>
#include "nvaccel_owner_policy.h"
#include "nvrm_gpuva_abi.h"
#define NVRM_SS_KERNEL_ABI 1
#include "nvrm_surfshare_abi.h"
using IOReturn=int; using task_t=void *; using SInt64=long long;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=1;
constexpr unsigned kMillisecondScale=1,THREAD_UNINT=0;
using IOSimpleLock=std::mutex; using IOLock=std::mutex;
static unsigned effects;
static IOLock *IOLockAlloc() { return new IOLock; }
static void IOLockFree(IOLock *l) { delete l; }
static bool OSCompareAndSwapPtr(void *old,void *value,void *volatile *p) { if(*p!=old)return false; *p=value;return true; }
static void IOSimpleLockLock(IOSimpleLock *l) { l->lock(); }
static void IOSimpleLockUnlock(IOSimpleLock *l) { l->unlock(); }
static void IOLockLock(IOLock *l) { l->lock(); }
static void IOLockUnlock(IOLock *l) { l->unlock(); }
static void IOLockWakeup(IOLock *,void *,bool) {}
static uint64_t now;
static uint64_t mach_absolute_time() { return now; }
static void clock_interval_to_deadline(unsigned ms,unsigned,uint64_t *d) { *d=now+ms; }
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) { *ns=t*1000000; }
static void IOLockSleepDeadline(IOLock *,void *,uint64_t d,unsigned) { now=d; }
static SInt64 OSAddAtomic64(SInt64 n,SInt64 *v) { return __atomic_fetch_add(v,n,__ATOMIC_RELAXED); }
static SInt64 gCntSurfMarks,gCntSurfWaits,gCntSurfTimeouts,gCntResFree;
static int gSurfWaitMaxUs,gSurfGpuOn=1;
struct OSObject { virtual ~OSObject()=default; mutable unsigned refs=1; void retain()const{++refs;} void release()const{assert(refs);--refs;} };
struct OSSymbol : OSObject { static OSSymbol *withCStringNoCopy(const char *) { ++effects; static OSSymbol s; return &s; } };
struct OSDictionary : OSObject {};
struct IOService : OSObject {
 static OSDictionary *serviceMatching(const char *) { ++effects; return nullptr; }
 static IOService *waitForMatchingService(OSDictionary *,unsigned long long) { ++effects; return nullptr; }
 int callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) { ++effects; return 0; }
};
struct Owner { bool handles(NVRMRmHandles *) { ++effects;return true; } };
struct NVAccel : IOService { bool fCandidate=true,fStopping=false; Owner *fOwner=nullptr; static NVAccel *resolve(NVAccel *a){return a;} };
struct Allocation { NVBackingKey identity={1,1}; NVBackingKey key()const{return identity;} };
struct NVVidMemory { Allocation allocation; Allocation *fBacking=&allocation; bool fScanout=false;
 void *handle(){return this;} unsigned long long capacity(){return 4096;} };
struct Descriptor { unsigned releases=0; void release(){++releases;} };
struct IOAccelResource2 : OSObject { unsigned freed=0; void free(){++freed;} };
struct NVDisplayPipe;
struct NVResource : IOAccelResource2 {
 NVVidMemory memory; Descriptor *fApDesc=nullptr; NVDisplayPipe *fFramebufferPipe=nullptr;
 void releaseFramebufferBacking(){if(fApDesc){fApDesc->release();fApDesc=nullptr;}}
 void free();
};
struct NVDisplayPipe { void destroyFramebufferResource(unsigned,NVResource *r){r->releaseFramebufferBacking();} };
static NVVidMemory *nvres_vidmem(NVResource *r){return r?&r->memory:nullptr;}
#define OSDynamicCast(T,p) dynamic_cast<T *>(p)
#define ALOG(...) ((void)0)
static void *gNVSVRoot;
static unsigned gNVSVOk,gNVSVRefused,gNVSVDirty,gNVSVDirtyRefused;
static void nvSVZero(void *p,unsigned long n){std::memset(p,0,n);}
static bool nvSVDiag(){return false;}
static void proc_selfname(char *p,unsigned long n){std::strncpy(p,"WindowServer",n);}
static unsigned nvAccelSurfDirty(void *,NVResource *,unsigned){++effects;return 0;}
static bool nvAccelRestoreParkedForDirty(NVResource *,NVVidMemory *,unsigned){++effects;return true;}
static void *_ZN13IOSurfaceRoot13lookupSurfaceEjP4task(void *,unsigned,task_t){++effects;return nullptr;}
static bool nvFamLock(NVAccel *,bool){++effects;return false;}
static void nvFamUnlock(NVAccel *){++effects;}
static void *_ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej(void *,void *,unsigned){++effects;return nullptr;}
static void _ZN16IOAccelVidMemory4wireEv(void *){++effects;}
static IOService *nvAccelNVRM(NVAccel *){++effects;return nullptr;}
static constexpr unsigned kNvResType=0;
'''
checks = r'''
static void pass(const char *id){std::printf("OPTC_CASE {\"id\":\"%s\",\"result\":\"PASS\"}\n",id);}
int main(){
 NVResource before; before.free(); assert(before.freed==1);
 IOSimpleLock tableLock; gNVSVPendLock=&tableLock;
 NVResource oldOwner,newOwner,unrelated;
 assert(nvSVPend(7,1,&oldOwner)); assert(nvSVPend(7,1,&newOwner));
 oldOwner.free(); assert(nvSVPend(7,2)); unrelated.free(); assert(nvSVPend(7,2));
 newOwner.free(); assert(!nvSVPend(7,2));
 NVResource reused; assert(nvSVPend(8,1,&reused));
 reused.memory.allocation.identity.allocationGeneration++;
 nvSVPend(8,0,&reused); assert(nvSVPend(8,2));
 assert(nvSVPend(8,1,&reused)); nvSVPend(8,0,&reused); assert(!nvSVPend(8,2));
 Descriptor d;
 for(unsigned id=1;id<=10000;++id){NVResource r;r.fApDesc=&d;assert(nvSVPend(id,1,&r));r.free();assert(r.freed==1&&!r.fApDesc&&!nvSVPend(id,2));}
 assert(d.releases==10000);
 NVResource owners[kNVSVPend];
 for(unsigned i=0;i<kNVSVPend;++i)assert(nvSVPend(i+1,1,&owners[i]));
 assert(!nvSVPend(kNVSVPend+1,1,&unrelated)); owners[17].free();assert(nvSVPend(kNVSVPend+1,1,&unrelated));
 unrelated.free();for(auto &r:owners)nvSVForgetResource(&r);
 std::thread threads[4];
 for(unsigned w=0;w<4;++w)threads[w]=std::thread([w]{NVResource r;for(unsigned i=0;i<1000;++i){assert(nvSVPend(100+w,1,&r));nvSVForgetResource(&r);}});
 for(auto &t:threads)t.join();
 for(unsigned i=0;i<kNVSVPend;++i)assert(!gNVSVPendId[i]&&!gNVSVPendResource[i]);
 IOLock waitLock;gSurfPendLock=&waitLock;
 for(unsigned i=1;i<=32;++i)assert(nvSurfMark(i,true));
 assert(!nvSurfMark(33,true));assert(nvSurfIsPending(1));
 assert(!nvSurfWaitReady(1));assert(nvSurfIsPending(1));
 gSurfGpuOn=0;assert(!nvSurfWaitReady(1));assert(nvSurfIsPending(1));
 assert(nvSurfMark(1,false));assert(nvSurfWaitReady(1));assert(nvSurfMark(33,true));
 pass("publication_failure");
 NVAccel candidate;
 for(unsigned cmd:{NVRM_ESC_SURF_VRAM,NVRM_ESC_SURF_DIRTY})
  for(unsigned plane:{0u,NVRM_SS_PLANE_GPU_MARK,NVRM_SS_PLANE_GPU_MARK|1u})
   for(unsigned id:{0u,42u}){
    NVRMSurfShareEsc q={};q.version=NVRM_SURFSHARE_VERSION;q.plane=plane;q.surfaceID=id;q.hSrcClient=q.hSrcMemory=0xffffffffu;
    assert(nvAccelSurfVram(&candidate,&q,(task_t)1,cmd)==kIOReturnSuccess);
    assert(q.status==NVRM_SS_OFF&&!q.hSrcClient&&!q.hSrcMemory&&!effects);
   }
 pass("private_publication_no_export");
}
'''
with tempfile.TemporaryDirectory(prefix="optc-pending-") as directory:
    path = pathlib.Path(directory)
    cpp = path / "test.cpp"
    cpp.write_text(fixture + table + waits + method("void NVResource::free()") +
                   method("static unsigned nvSurfGpuEscape(") +
                   method("static IOReturn nvAccelSurfVram(") + checks)
    binary = path / "test"
    subprocess.run(["clang++", "-std=c++17", "-pthread", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(root / "kexts/NVRM/accel"), "-I" + str(root / "kexts/NVRM"),
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
