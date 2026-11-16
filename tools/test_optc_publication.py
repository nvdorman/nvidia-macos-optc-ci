#!/usr/bin/env python3
"""Exercise stock producer's checked completion and actual NVK tail emission policy."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
events = (root / 'plugin/NVMTLEventSync.m').read_text()
objects = (root / 'plugin/NVMTLObjects.m').read_text()
def method(text, start):
    first = text.index(start)
    return text[first:text.index('\n}', first) + 2]
probe = events[events.index('static _Atomic int nvmtl_ws_marks_on'):events.index('static const void *const nvmtl_ws_marks_key')]
commit = method(objects, '- (BOOL)commitAndWaitUntilSubmitted {')
finish = method(events, '- (void)nvmtlFinishExecution:(NSError *)error {')
fixture = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
static const char *test_progname(void){return "WindowServer";}
#define getprogname test_progname
static int nvmtl_vk_surface_gpu_mark(uint32_t surface,int pending){assert(!surface&&!pending);return 1;}
static void nvlog(const char *format,...){(void)format;}
struct Commands {int unused;};
@interface NVMTLSubmissionPart:NSObject {@public void *inflight;struct Commands commands;} @end
@implementation NVMTLSubmissionPart @end
@interface NVMTLTexture:NSObject {@public NVMTLTexture *_parent;} @end
@implementation NVMTLTexture @end
@interface NVMTLResSet:NSObject
- (void)removeAllObjects;
@end
@implementation NVMTLResSet
- (void)removeAllObjects {}
@end
@class NVMTLCommandBuffer;
static NSMutableArray *nvmtl_quarantined_parts;
static int nvmtl_gsub_finish(NVMTLSubmissionPart *part){(void)part;return 0;}
static void nvmtl_present_dump(NVMTLTexture *texture){(void)texture;}
static int nvmtl_vk_interval_seconds(void *timing,double *a,double *b){(void)timing;*a=*b=0;return 0;}
static void nvmtl_surface_finish_writes(NVMTLCommandBuffer *buffer){(void)buffer;}
static void nvmtl_busy_mark_done(NVMTLCommandBuffer *buffer){(void)buffer;}
static void nvmtl_res2_cb_done(NVMTLCommandBuffer *buffer){(void)buffer;}
@interface NVMTLCommandBuffer:NSObject {
@public
    BOOL _partsRetiring,_scheduleAfterPublish,_gpuTimingInvalid,_gpuTimingSeen,_executionDone;
    NSUInteger _nextPart,_e78RingOff;
    NSMutableArray *_parts,*_presentations,*_scratch,*_stTwins;
    NVMTLTexture *_present;NSCondition *_completionCondition;
    NSError *_cbError,*plannedError;MTLCommandBufferStatus _cbstatus;
    double _gpuStart,_gpuEnd,_legacyKernelEnd;int _gpuTiming;
    id _e78Ring;void *_g79BRing[2];NVMTLResSet *_resources;NSMutableDictionary *_surfOverwrites;
    unsigned completeWaits,scheduledWaits;
}
- (void)nvmtlMarkScheduled;
- (void)nvmtlCollectDeviceTiming:(struct Commands *)commands;
- (void)nvmtlReleaseCapacity;
- (void)nvmtlFinishExecution:(NSError *)error;
- (BOOL)commitAndWaitUntilSubmitted;
- (void)commit;
- (void)waitUntilCompleted;
- (void)waitUntilScheduled;
- (MTLCommandBufferStatus)status;
@end
'''
implementation = r'''
@implementation NVMTLCommandBuffer
- (instancetype)init {if((self=[super init])){_parts=[NSMutableArray new];_presentations=[NSMutableArray new];_completionCondition=[NSCondition new];}return self;}
- (void)nvmtlMarkScheduled {}
- (void)nvmtlCollectDeviceTiming:(struct Commands *)commands {(void)commands;}
- (void)nvmtlReleaseCapacity {}
- (void)commit {[self nvmtlFinishExecution:plannedError];}
- (void)waitUntilCompleted {assert(_executionDone);completeWaits++;}
- (void)waitUntilScheduled {scheduledWaits++;}
- (MTLCommandBufferStatus)status {return _cbstatus;}
'''
checks = r'''
@end
int main(void){@autoreleasepool{
    unsetenv("NVMTL_WS_WAIT_COMPLETION");setenv("NVMTL_PRESENT_INLINE","1",1);
    __block unsigned shown=0;
    NVMTLCommandBuffer *good=[NVMTLCommandBuffer new];
    [good->_presentations addObject:[^{shown++;} copy]];
    assert([good commitAndWaitUntilSubmitted]);
    assert(good->completeWaits==1&&good->scheduledWaits==0&&shown==1&&good->_cbstatus==MTLCommandBufferStatusCompleted);
    for(NSString *why in @[@"GPU submit failed",@"readback failed",@"IOSurfaceUnlock failed"]){
        NVMTLCommandBuffer *bad=[NVMTLCommandBuffer new];
        bad->plannedError=[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal userInfo:@{NSLocalizedDescriptionKey:why}];
        [bad->_presentations addObject:[^{shown++;} copy]];
        assert(![bad commitAndWaitUntilSubmitted]);
        assert(bad->completeWaits==1&&bad->scheduledWaits==0&&shown==1&&bad->_cbstatus==MTLCommandBufferStatusError);
        assert(bad->_presentations.count==0&&bad->_executionDone);
    }
    puts("OPTC_CASE {\"id\":\"producer_success\",\"result\":\"PASS\"}");
    puts("OPTC_CASE {\"id\":\"publication_failure\",\"result\":\"PASS\"}");
}return 0;}
'''
patch = (root / 'nvk/nvk-macos.patch').read_text()
tail_source = '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
start = tail_source.index('static void\nwrite_l2_sysmem_tail(')
tail = tail_source[start:tail_source.index('\n}', start) + 2]
c_fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
struct nv_push {unsigned ops[8],count;};
struct cword {unsigned membar_type;};struct dword {unsigned operation;};
enum {NVC86F_MEM_OP_D_OPERATION_MEMBAR=1,NVC86F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY=2,NVC86F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE=3,MEMBAR_TYPE_SYS_MEMBAR=4};
static const char *setting;
static const char *nvmtl_cfg_getenv(const char *name){(void)name;return setting;}
#define P_IMMD(p,c,m,v) ((p)->ops[(p)->count++]=9)
#define P_MTHD(p,c,m) ((void)(p))
#define P_NVC86F_MEM_OP_A(p,v) ((void)(p))
#define P_NVC86F_MEM_OP_B(p,v) ((void)(p))
#define P_NVC86F_MEM_OP_C(p,v) assert(((struct cword)v).membar_type==MEMBAR_TYPE_SYS_MEMBAR)
#define P_NVC86F_MEM_OP_D(p,v) ((p)->ops[(p)->count++]=((struct dword)v).operation)
'''
c_checks = r'''
int main(int argc,char **argv){
    assert(argc==2);setting=argv[1][0]=='u'?0:argv[1];struct nv_push push={0};write_l2_sysmem_tail(&push);
    const unsigned mode=setting?(unsigned)(setting[0]-'0'):2;
    if(mode==0)assert(push.count==0);
    else if(mode==3)assert(push.count==2&&push.ops[0]==9&&push.ops[1]==3);
    else {
        assert(push.ops[0]==9&&push.ops[1]==1&&push.ops[push.count-1]==1);
        assert(push.count==(mode==1?5u:4u));
        assert(push.ops[push.count-2]==3);
        if(mode==1)assert(push.ops[2]==2);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='optc-publication-') as directory:
    path = Path(directory)
    (path / 'test.m').write_text(fixture + probe + implementation + finish + '\n' + commit + checks)
    subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-fblocks', '-framework', 'Foundation', '-framework', 'Metal',
                    str(path / 'test.m'), '-o', str(path / 'publication')], check=True)
    subprocess.run([str(path / 'publication')], check=True)
    (path / 'tail.c').write_text(c_fixture + tail + c_checks)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    str(path / 'tail.c'), '-o', str(path / 'tail')], check=True)
    for mode in ('unset', '0', '1', '2', '3'):
        subprocess.run([str(path / 'tail'), mode], check=True)
    print('OPTC_CASE {"id":"nvk_host_visibility","result":"PASS"}')
