#!/usr/bin/env python3
"""Run production PM/dispatch lease and mapping-receipt methods without a GPU."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kexts/NVRM/NVRM.cpp').read_text()
start = source.index('IOService *NVRM::copyAccelTarget()')
methods = source[start:source.index('IOReturn NVRM::callPlatformFunction', start)]
map_source = (root / 'kexts/NVRM/os-xnu.cpp').read_text()
start = map_source.index('bool nv_xnu_kernel_mapping_info(')
map_method = map_source[start:map_source.index('\nvoid NV_API_CALL os_unmap_kernel_space', start)]
fixture = r'''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include "nvrm_owner_abi.h"
using SInt32=int32_t;using IOOptionBits=unsigned;using NvU64=uint64_t;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotReady=2,
    kIOReturnNotPermitted=3,kIOReturnNotFound=4,kIOReturnExclusiveAccess=5;
constexpr unsigned kIOMapCacheMask=0xf00,kIOMapInhibitCache=0x100,kIOMapWriteCombineCache=0x400;
#define THREAD_UNINT 0
struct IOLock {std::mutex mutex;std::condition_variable cv;};
static std::mutex sleepGuard;static std::condition_variable slept;static bool isSleeping=false;
static void IOLockLock(IOLock *l){l->mutex.lock();}
static void IOLockUnlock(IOLock *l){l->mutex.unlock();}
static void IOLockWakeup(IOLock *l,void*,bool){l->cv.notify_all();}
static void IOLockSleep(IOLock *l,void*,int){
    {std::lock_guard<std::mutex> g(sleepGuard);isSleeping=true;slept.notify_all();}
    std::unique_lock<std::mutex> g(l->mutex,std::adopt_lock);l->cv.wait(g);g.release();
}
static SInt32 OSAddAtomic(SInt32 n,volatile SInt32 *p){return __atomic_fetch_add(p,n,__ATOMIC_SEQ_CST);}
struct OSObject {std::atomic<unsigned> refs{1};virtual ~OSObject()=default;void retain(){++refs;}void release(){assert(refs.fetch_sub(1)>1);}};
struct OSNumber:OSObject {uint64_t value=0;uint64_t unsigned64BitValue(){return value;}};
#define OSDynamicCast(T,x) dynamic_cast<T*>(x)
class IOService:public OSObject {
public:
    IOService *provider=nullptr;bool inactive=false,accel=false;OSNumber generation;
    IOService *getProvider(){return provider;}bool isInactive(){return inactive;}
    void *metaCast(const char *name){return accel&&!strcmp(name,"NVAccel")?this:nullptr;}
    OSObject *copyProperty(const char *key){assert(!strcmp(key,NVRM_OWNER_GENERATION_PROPERTY));generation.retain();return &generation;}
};
static IOLock pmLock;static IOLock *gNvPmGate=&pmLock;
static std::atomic<unsigned> pmUsers{0};static bool pmOpen=true;
static bool nv_xnu_pm_try_enter(int *counted){*counted=0;if(!pmOpen)return false;++pmUsers;*counted=1;return true;}
static void nv_xnu_pm_exit(int counted){if(counted)assert(pmUsers.fetch_sub(1)>0);}
class NVRM:public IOService {
public:
    IOLock ownedLock;IOLock *fOwnerLock=&ownedLock;IOService *fPCI=nullptr,*fAccelTarget=nullptr;
    uint64_t fAccelGeneration=0;unsigned fOwnerCpuUsers=0;bool fOwnerCpuClosed=false,fOwnerStopping=false;
    bool fAdapter=true;unsigned fPassDone=2;volatile SInt32 fGpuPm=0;enum{kGpuPmRunning=0};
    IOService *copyAccelTarget();IOReturn updateAccelTarget(const NVRMAccelTarget *);
    IOReturn ownerCpuAccess(const NVRMOwnerCpuAccess *);void closeOwnerCpuAccess(bool);
};
struct IOMemoryDescriptor {};
struct IOMemoryMap {uint64_t length;unsigned options;uint64_t getLength(){return length;}unsigned getMapOptions(){return options;}};
struct nv_xnu_map {IOMemoryDescriptor *md;IOMemoryMap *map;void *va;NvU64 phys,size;void *alias;};
static nv_xnu_map nv_xnu_maps[4];static unsigned nv_xnu_maps_high=1;
static IOLock mappingLock;static IOLock *nv_xnu_maps_lock=&mappingLock;
'''
checks = r'''
struct Body {std::mutex lock;std::condition_variable cv;bool entered=false,finish=false;};
static IOReturn blockingBody(void *context){
    auto &b=*static_cast<Body*>(context);assert(pmUsers>0);
    std::unique_lock<std::mutex> lock(b.lock);b.entered=true;b.cv.notify_all();b.cv.wait(lock,[&]{return b.finish;});return 73;
}
static IOReturn instantBody(void *context){++*static_cast<unsigned*>(context);return 0;}
int main(){
    NVRM n;IOService pci,wrongPci,target,replacement,owner;n.fPCI=&pci;target.provider=&pci;target.accel=true;
    replacement.provider=&wrongPci;replacement.accel=true;owner.provider=&n;owner.generation.value=9;
    NVRMAccelTarget r{1,sizeof(r),NVRM_ACCEL_TARGET_REGISTER,0,&target,4};
    assert(n.updateAccelTarget(&r)==0&&target.refs==2);assert(n.updateAccelTarget(&r)==0&&target.refs==2);
    IOService *borrow=n.copyAccelTarget();assert(borrow==&target&&target.refs==3);
    r.target=&replacement;assert(n.updateAccelTarget(&r)==kIOReturnNotPermitted&&replacement.refs==1);
    replacement.provider=&pci;assert(n.updateAccelTarget(&r)==kIOReturnExclusiveAccess&&replacement.refs==1);
    r.target=&target;r.operation=NVRM_ACCEL_TARGET_UNREGISTER;r.generation=5;
    assert(n.updateAccelTarget(&r)==kIOReturnNotFound&&n.copyAccelTarget()==&target);target.release();
    r.generation=4;assert(n.updateAccelTarget(&r)==0&&n.copyAccelTarget()==nullptr&&target.refs==2);
    borrow->release();assert(target.refs==1);
    unsigned calls=0;NVRMOwnerCpuAccess u{1,sizeof(u),&owner,9,instantBody,&calls};
    assert(n.ownerCpuAccess(&u)==0&&calls==1&&pmUsers==0&&owner.refs==1);
    u.ownerGeneration=8;assert(n.ownerCpuAccess(&u)==kIOReturnNotPermitted&&calls==1);u.ownerGeneration=9;
    pmOpen=false;assert(n.ownerCpuAccess(&u)==kIOReturnNotReady&&calls==1);pmOpen=true;
    Body body;u.body=blockingBody;u.context=&body;
    std::thread access([&]{assert(n.ownerCpuAccess(&u)==73);});
    {std::unique_lock<std::mutex> l(body.lock);body.cv.wait(l,[&]{return body.entered;});}
    std::atomic<bool> drained{false};std::thread stop([&]{n.closeOwnerCpuAccess(true);drained=true;});
    {std::unique_lock<std::mutex> l(sleepGuard);slept.wait(l,[]{return isSleeping;});}
    assert(!drained&&owner.refs==2&&pmUsers==1);
    NVRMOwnerCpuAccess later{1,sizeof(later),&owner,9,instantBody,&calls};
    assert(n.ownerCpuAccess(&later)==kIOReturnNotReady&&calls==1);
    {std::lock_guard<std::mutex> l(body.lock);body.finish=true;body.cv.notify_all();}
    access.join();stop.join();assert(drained&&pmUsers==0&&owner.refs==1&&n.fOwnerCpuUsers==0);
    r.operation=NVRM_ACCEL_TARGET_REGISTER;assert(n.updateAccelTarget(&r)==kIOReturnNotReady);
    puts("OPTC_CASE {\"id\":\"power_lease\",\"result\":\"PASS\"}");
    puts("OPTC_CASE {\"id\":\"dispatch_drain\",\"result\":\"PASS\"}");
    IOMemoryDescriptor md;IOMemoryMap map{4096,kIOMapWriteCombineCache};
    nv_xnu_maps[0]={&md,&map,reinterpret_cast<void*>(0x10000),0x20000,4096,nullptr};
    NVRMKernelMapInfo q{1,sizeof(q),reinterpret_cast<void*>(0x10040),128,0,0,0,0};
    assert(nv_xnu_kernel_mapping_info(&q)&&q.physical==0x20040&&q.availableBytes==4032&&q.cacheOptions==kIOMapWriteCombineCache);
    q.requiredBytes=4033;assert(!nv_xnu_kernel_mapping_info(&q)&&!q.physical&&!q.availableBytes);
    q.requiredBytes=128;map.options=0;assert(!nv_xnu_kernel_mapping_info(&q));map.options=kIOMapInhibitCache;
    assert(nv_xnu_kernel_mapping_info(&q));nv_xnu_maps[0].alias=&md;assert(!nv_xnu_kernel_mapping_info(&q));nv_xnu_maps[0].alias=nullptr;
    nv_xnu_maps[0].phys=UINT64_MAX-3;assert(!nv_xnu_kernel_mapping_info(&q));
    nv_xnu_maps[0].phys=0x20000;map.length=64;assert(!nv_xnu_kernel_mapping_info(&q));
    q.version=2;assert(!nv_xnu_kernel_mapping_info(&q));
    puts("OPTC_CASE {\"id\":\"mapping_receipt\",\"result\":\"PASS\"}");
}
'''
with tempfile.TemporaryDirectory(prefix='optc-backend-') as directory:
    path = Path(directory)
    (path / 'IOKit').mkdir()
    (path / 'IOKit/IOReturn.h').write_text('#pragma once\n#include <stdint.h>\ntypedef int32_t IOReturn;\n')
    cpp = path / 'test.cpp'
    cpp.write_text(fixture + methods + map_method + checks)
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-pthread', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(path), '-I', str(root / 'kexts/NVRM'), str(cpp), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
