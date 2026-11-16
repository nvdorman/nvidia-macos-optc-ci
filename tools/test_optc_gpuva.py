#!/usr/bin/env python3
"""Execute production GPUVA allocation/retirement against deterministic RM failures."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kexts/NVRM/nvrm_gpuva.cpp').read_text()
body = source[source.index('#define NVRM_CLS_FERMI_VASPACE_A'):]
fixture = r'''
#include <cassert>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "nvrm_gpuva_abi.h"
using NvU32=uint32_t; using NvU64=uint64_t; using NvV32=uint32_t;
using NvUPtr=uintptr_t; using NvP64=uintptr_t; using nvidia_stack_t=void;
#define NV_API_CALL
#define kprintf std::printf
#define NV_ERR_INVALID_STATE 1u
#define NV_ERR_NO_MEMORY 2u
#define NV_ERR_INVALID_ARGUMENT 3u
#define NV_ERR_INSUFFICIENT_RESOURCES 4u
#define NV_ERR_GENERIC 5u
#define NV_ERR_STATE_IN_USE 6u
#define NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS 1u
#define NVOS64_FLAGS_NONE 0u
#define NVOS32_TYPE_IMAGE 1u
#define NVOS32_ALLOC_FLAGS_VIRTUAL 1u
#define NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE 2u
#define DRF_DEF(a,b,c,d) 0u
using IOLock=std::mutex;
static IOLock *IOLockAlloc(){return new IOLock;}
static void IOLockFree(IOLock *l){delete l;}
static void IOLockLock(IOLock *l){l->lock();}
static void IOLockUnlock(IOLock *l){l->unlock();}
static bool OSCompareAndSwapPtr(void *old,void *value,void *volatile *p){return __sync_bool_compare_and_swap(p,old,value);}
struct NV_VASPACE_ALLOCATION_PARAMETERS { uint32_t flags; };
struct NV_MEMORY_ALLOCATION_PARAMS { uint32_t owner,type,flags,hVASpace; uint64_t size,alignment,offset; };
enum {NV04_ALLOC=1,NV01_FREE,NV04_MAP_MEMORY_DMA,NV04_UNMAP_MEMORY_DMA};
struct nvidia_kernel_rmapi_ops_t {
    unsigned op;
    struct {
        struct {uint32_t hRoot,hObjectParent,hObjectNew,hClass; uintptr_t pAllocParms,pRightsRequested; uint32_t paramsSize,flags,status;} alloc;
        struct {uint32_t hRoot,hObjectParent,hObjectOld,status;} free;
        struct {uint32_t hClient,hDevice,hDma,hMemory; uint64_t offset,length; uint32_t flags,status; uint64_t dmaOffset;} mapMemoryDma;
        struct {uint32_t hClient,hDevice,hDma,hMemory,flags; uint64_t dmaOffset; uint32_t status;} unmapMemoryDma;
    } params;
};
struct Device {unsigned client,device;};
extern "C" int nvrm_kapi_dev_handles(const void *p,unsigned *c,unsigned *d,unsigned *s){
    if(!p)return 0; auto x=static_cast<const Device*>(p); *c=x->client;*d=x->device;*s=3;return 1;
}
extern "C" unsigned nvrm_kapi_mem_handle(const void *p){return unsigned(reinterpret_cast<uintptr_t>(p));}
static std::mutex rmLock;
static unsigned nextHandle=10, allocFailClass=0, freeFailHandle=0, unmapFail=0, freeCalls=0;
static std::map<unsigned,unsigned> live;
extern "C" void rm_kernel_rmapi_op(nvidia_stack_t *,void *p){
    std::lock_guard<std::mutex> lock(rmLock);
    auto &o=*static_cast<nvidia_kernel_rmapi_ops_t*>(p);
    if(o.op==NV04_ALLOC){
        auto &a=o.params.alloc;
        if(allocFailClass==a.hClass){allocFailClass=0;a.status=99;return;}
        a.hObjectNew=++nextHandle;live[a.hObjectNew]=a.hClass;a.status=0;
        if(a.hClass==0x50a0)reinterpret_cast<NV_MEMORY_ALLOCATION_PARAMS*>(a.pAllocParms)->offset=uint64_t(a.hObjectNew)<<16;
    } else if(o.op==NV01_FREE){
        auto &f=o.params.free;++freeCalls;
        if(freeFailHandle==f.hObjectOld){freeFailHandle=0;f.status=98;return;}
        f.status=live.erase(f.hObjectOld)?0:97;
    } else if(o.op==NV04_MAP_MEMORY_DMA){
        auto &m=o.params.mapMemoryDma;m.status=live.count(m.hDma)?0:96;m.dmaOffset=uint64_t(m.hDma)<<16;
    } else {
        auto &u=o.params.unmapMemoryDma;u.status=unmapFail?95:0;unmapFail=0;
    }
}
'''
checks = r'''
static NVRMGpuVaRequest request(Device &d){NVRMGpuVaRequest r={};r.version=1;r.kapiDevice=&d;r.size=4096;r.align=4096;return r;}
int main(){
    Device d{1,2};
    std::mutex barrier;std::condition_variable changed;unsigned allocated=0;bool release=false;
    std::vector<std::thread> workers;
    for(unsigned i=0;i<8;++i)workers.emplace_back([&]{
        auto r=request(d);assert(nvrm_gpuva_alloc(&r)==0);
        {std::unique_lock<std::mutex> l(barrier);++allocated;changed.notify_all();changed.wait(l,[&]{return release;});}
        assert(nvrm_gpuva_free(&r)==0);assert(!r.hVirt);
    });
    {std::unique_lock<std::mutex> l(barrier);changed.wait(l,[&]{return allocated==8;});}
    auto drain=request(d);assert(nvrm_gpuva_drain(&drain)!=0&&drain.status==NV_ERR_STATE_IN_USE);
    {std::lock_guard<std::mutex> l(barrier);release=true;changed.notify_all();}
    for(auto &w:workers)w.join();
    assert(nvrm_gpuva_drain(&drain)==0&&gSpaceCount==0&&live.empty());
    allocFailClass=NVRM_CLS_NV50_MEMORY_VIRTUAL;
    auto bad=request(d);assert(nvrm_gpuva_alloc(&bad)!=0);assert(gSpaces[0].reservations==0);
    assert(nvrm_gpuva_drain(&drain)==0&&live.empty());
    auto r=request(d);assert(nvrm_gpuva_alloc(&r)==0);r.kapiMemory=reinterpret_cast<void*>(0x55);r.isVidmem=1;
    assert(nvrm_gpuva_map(&r)==0);auto before=r;unsigned frees=freeCalls;
    unmapFail=1;assert(nvrm_gpuva_free(&r)!=0);
    assert(r.hMemory==before.hMemory&&r.hVirt==before.hVirt&&r.gpuva==before.gpuva&&freeCalls==frees);
    assert(nvrm_gpuva_drain(&drain)!=0);
    freeFailHandle=r.hVirt;assert(nvrm_gpuva_free(&r)!=0);
    assert(r.hMemory==0&&r.hVirt==before.hVirt&&gSpaces[0].reservations==1);
    assert(nvrm_gpuva_free(&r)==0&&r.hVirt==0&&gSpaces[0].reservations==0);
    freeFailHandle=gSpaces[0].hVaSpace;assert(nvrm_gpuva_drain(&drain)!=0&&gSpaceCount==1);
    assert(nvrm_gpuva_drain(&drain)==0&&gSpaceCount==0&&live.empty());
    for(unsigned epoch=0;epoch<20;++epoch){
        d.client=100+epoch;auto q=request(d);auto t=request(d);
        assert(nvrm_gpuva_alloc(&q)==0);q.kapiMemory=reinterpret_cast<void*>(0x66);assert(nvrm_gpuva_map(&q)==0);
        q.flags=NVRM_GPUVA_FLAG_UNMAP_ONLY;assert(nvrm_gpuva_free(&q)==0&&q.hVirt&&q.hMemory==0);
        assert(nvrm_gpuva_drain(&t)!=0);q.flags=0;assert(nvrm_gpuva_free(&q)==0);
        assert(nvrm_gpuva_drain(&t)==0&&gSpaceCount==0&&live.empty());
    }
    IOLockFree(gSpaceLock);gSpaceLock=nullptr;
    puts("OPTC_CASE {\"id\":\"gpuva_retire\",\"result\":\"PASS\"}");
}
'''
with tempfile.TemporaryDirectory(prefix='optc-gpuva-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    cpp.write_text(fixture + body + checks)
    compiler = os.environ.get('CXX', 'clang++')
    subprocess.run([compiler, '-std=c++17', '-pthread', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'kexts/NVRM'), str(cpp), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
