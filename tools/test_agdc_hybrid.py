#!/usr/bin/env python3
"""Verify AGDC hybrid (NMIntelFB / NMGopFB) slot planning, enrollment, and K5 timing."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "nvrm-agdc-hybrid.h"
#include "nvrm-agdc-k5.h"
#include "intelfb_console.h"

static void casePass(const char *id) {
    std::printf("OPTC_CASE {\"id\":\"%s\",\"result\":\"PASS\"}\n", id);
}

static nvrm_agdc_candidate makeNative(uint64_t rid, uint32_t depIdx, int hasAccel = 0, uint32_t accelIdx = 0) {
    nvrm_agdc_candidate c{};
    c.registry_id = rid;
    c.kind = NVRM_AGDC_FB_NATIVE;
    c.has_dep_index = 1;
    c.dep_index = depIdx;
    c.has_accel_index = hasAccel;
    c.accel_index = accelIdx;
    return c;
}

static nvrm_agdc_candidate makeHybrid(uint64_t rid, int hasAccel = 0, uint32_t accelIdx = 0,
                                      int matchesHead4 = 0, int hasDense = 0, uint32_t dense = 0) {
    nvrm_agdc_candidate c{};
    c.registry_id = rid;
    c.kind = NVRM_AGDC_FB_HYBRID;
    c.has_dep_index = 1;
    c.dep_index = 0; // NMIntelFB publishes independent Intel local index 0
    c.has_accel_index = hasAccel;
    c.accel_index = accelIdx;
    c.matches_head4 = matchesHead4;
    c.has_head4_dense = hasDense;
    c.head4_dense = dense;
    return c;
}

static void testSoloHybridAndK5() {
    // 1a. Solo NMIntelFB already enrolled in IOAccelDisplayPipe (dense=0)
    nvrm_agdc_candidate c0 = makeHybrid(0x10004e4dULL, 1, 0, 1, 1, 0);
    nvrm_agdc_plan p0{};
    nvrm_agdc_plan_slots(&c0, 1, NVRM_AGDC_MAX_SLOTS, &p0);
    assert(p0.count == 1 && p0.slot_candidate[0] == 0);
    assert(!p0.left_out_dup && !p0.left_out_hole && !p0.left_out_range && !p0.left_out_noidx);

    // 1b. Solo NMIntelFB before IOAccelIndex appears still claims slot 0
    nvrm_agdc_candidate c1 = makeHybrid(0x10004e4dULL);
    nvrm_agdc_plan p1{};
    nvrm_agdc_plan_slots(&c1, 1, NVRM_AGDC_MAX_SLOTS, &p1);
    assert(p1.count == 1 && p1.slot_candidate[0] == 0);

    // 1c. Verify NVRMTiming from NMIntelFB satisfies AGDC K5 queries (0x921 & 0x711) on head[0]
    uint32_t t0[NM_INTEL_NVRMTIMING_WORDS] = {};
    assert(nm_intelfb_build_nvrm_timing(2560, 1600, 2608, 2640, 2720, 1603, 1609, 1650,
                                        1077120000ULL, 240000U, 1U, 1U, t0) == 1);
    nvk5_head heads[1] = {{t0, sizeof(t0)}};
    NVAGDCLinkConfig link{};
    int why = -1;
    int32_t idx = -1;
    assert(nvk5_answer_heads(NVK5_CMD_LINKCONFIG, &link, sizeof(link), &link, sizeof(link),
                             heads, 1, &why, &idx) == NVK5_R_LINK);
    assert(idx == 0 && link.port == 1 && link.timing.hActive == 2560 && link.timing.vActive == 1600);
    NVAGDCFBExtCap cap{};
    cap.type = NVK5_CAP_PLANESCALER;
    assert(nvk5_answer_heads(NVK5_CMD_FBCAPEX, &cap, sizeof(cap), &cap, sizeof(cap),
                             heads, 1, &why, &idx) == NVK5_R_CAPS);
    assert(idx == 0 && cap.count == 1);
    casePass("hybrid_solo_slot0_and_k5");
}

static void testCollisionFreeHybridPlusNative() {
    // 2a. External HDMI (dep=0, accel=0) + NMIntelFB (dep=0, accel=1): zero dep=0 collision
    nvrm_agdc_candidate a[2] = {
        makeNative(0xaa001, 0, 1, 0),
        makeHybrid(0xbb002, 1, 1, 1, 1, 1),
    };
    nvrm_agdc_plan pa{};
    nvrm_agdc_plan_slots(a, 2, NVRM_AGDC_MAX_SLOTS, &pa);
    assert(pa.count == 2 && pa.slot_candidate[0] == 0 && pa.slot_candidate[1] == 1 && !pa.left_out_dup);

    // 2b. Same pair BEFORE IOAccelIndex exists: hybrid must not collide at dep_index=0
    nvrm_agdc_candidate b[2] = {
        makeNative(0xaa001, 0),
        makeHybrid(0xbb002),
    };
    nvrm_agdc_plan pb{};
    nvrm_agdc_plan_slots(b, 2, NVRM_AGDC_MAX_SLOTS, &pb);
    assert(pb.count == 2 && pb.slot_candidate[0] == 0 && pb.slot_candidate[1] == 1 && !pb.left_out_dup);

    // 2c. Hotplug case: NMIntelFB already holds IOAccelIndex=0, new native FB arrives without IOAccelIndex (dep=0)
    nvrm_agdc_candidate c[2] = {
        makeNative(0xaa001, 0),
        makeHybrid(0xbb002, 1, 0, 1, 1, 0),
    };
    nvrm_agdc_plan pc{};
    nvrm_agdc_plan_slots(c, 2, NVRM_AGDC_MAX_SLOTS, &pc);
    assert(pc.count == 2 && pc.slot_candidate[0] == 1 && pc.slot_candidate[1] == 0 && !pc.left_out_dup);
    casePass("hybrid_plus_native_collision_free");
}

static void testFiveSlotsAndHolePreservation() {
    // 3a. Stock native hole (dep=0, dep=2) must still stop at count=1 and report left_out_hole=1
    nvrm_agdc_candidate h[2] = { makeNative(0x1, 0), makeNative(0x2, 2) };
    nvrm_agdc_plan ph{};
    nvrm_agdc_plan_slots(h, 2, NVRM_AGDC_MAX_SLOTS, &ph);
    assert(ph.count == 1 && ph.slot_candidate[0] == 0 && ph.left_out_hole == 1);

    // 3b. Full 4 native + 1 hybrid = 5 slots, 6th overflows into left_out_range
    nvrm_agdc_candidate f[6] = {
        makeNative(0x10, 0), makeNative(0x11, 1), makeNative(0x12, 2), makeNative(0x13, 3),
        makeHybrid(0x14, 1, 4, 1, 1, 4),
        makeNative(0x15, 5),
    };
    nvrm_agdc_plan pf{};
    nvrm_agdc_plan_slots(f, 6, NVRM_AGDC_MAX_SLOTS, &pf);
    assert(pf.count == 5 && pf.slot_candidate[4] == 4 && pf.left_out_range == 1);
    casePass("five_slots_and_hole_preservation");
}

static void testEnrollmentGate() {
    assert(nvrm_agdc_should_enroll_hybrid(0, 0x100, 0x100, 1, 1) == 0);
    assert(nvrm_agdc_should_enroll_hybrid(1, 0x100, 0, 1, 1) == 0);
    assert(nvrm_agdc_should_enroll_hybrid(1, 0x100, 0x200, 1, 1) == 0);
    assert(nvrm_agdc_should_enroll_hybrid(1, 0x100, 0x100, 0, 0) == 1);
    assert(nvrm_agdc_should_enroll_hybrid(1, 0, 0x100, 0, 0) == 0);
    assert(nvrm_agdc_should_enroll_hybrid(1, 0, 0x100, 1, 0) == 1);
    assert(nvrm_agdc_should_enroll_hybrid(1, 0, 0x100, 0, 1) == 1);
    casePass("hybrid_enrollment_gate");
}

int main() {
    testSoloHybridAndK5();
    testCollisionFreeHybridPlusNative();
    testFiveSlotsAndHolePreservation();
    testEnrollmentGate();
    std::puts("PASS: AGDC hybrid discovery, slot planning, and K5 timing");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="agdc-hybrid-") as td:
    work = pathlib.Path(td)
    src = work / "agdc_hybrid.cpp"
    exe = work / "agdc_hybrid"
    src.write_text(HARNESS)
    subprocess.run([
        os.environ.get("CXX", "clang++"), "-std=c++17", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(ROOT / "kexts/NVRMAGDC"),
        "-I", str(ROOT / "kexts/NMIntelFB"),
        str(src), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "halt_on_error=1",
                        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
