#!/usr/bin/env python3
"""Compile production topology routing and rejection cases.

Startup/unwind is covered by test_optc_console.py and the native CI instruction
contract, not by matching implementation spelling in the source.
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
fails = []


def arm(name, ok):
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        fails.append(name)


def run_compiled_routing_tests():
    cc = shutil.which("cc") or shutil.which("clang")
    if not cc:
        arm("host C compiler available for production routing tests", False)
        return
    src = r"""
#include <stdio.h>
#include "nvrm_fb_topology.h"
#include "nvaccel_optc_policy.h"

static int fails = 0;
#define CHK(name, cond) do { if (!(cond)) { printf("FAIL %s\n", name); fails++; } } while (0)

int main(void) {
    /* 1. NMIntelFB routes ONLY via independent Intel controller ID + local index 0 -> logical 4 */
    CHK("Intel native local0 -> logical 4",
        nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL, 1, 0u) == 4);
    /* 2. Misrouting controls: Intel with NVIDIA ID, local index 4, missing props -> rejected (-1) */
    CHK("Intel with shared NVIDIA ID rejected",
        nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u) == -1);
    CHK("Intel with legacy local 4 rejected (no shim)",
        nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 4u) == -1 &&
        nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 4u) == -1);
    CHK("Intel with missing props rejected (never defaults to 0)",
        nvrm_fb_logical_head(NVRM_FB_INTEL, 0, NVRM_FB_CONTROLLER_ID_INTEL, 1, 0u) == -1 &&
        nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL, 0, 0u) == -1);
    /* 3. Disjoint controllers: NVIDIA local 0 -> fFB, Intel local 0 -> logical 4 (never sets fFB) */
    CHK("only NVIDIA logical 0 may set primary fFB",
        nvrm_fb_logical_head(NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u) == 0 &&
        nm_optc_should_set_primary_fb(NVRM_FB_NVIDIA, 0) == 1 &&
        nm_optc_should_set_primary_fb(NVRM_FB_NVIDIA, 1) == 0 &&
        nm_optc_should_set_primary_fb(NVRM_FB_INTEL,  4) == 0 &&
        nm_optc_should_set_primary_fb(NVRM_FB_INTEL,  0) == 0 &&
        nm_optc_should_set_primary_fb(NVRM_FB_GOP,    4) == 0);
    /* 4. NMGopFB preserves legacy routing boundary only (rejects Intel ID and local 0..3) */
    CHK("NMGopFB rejects Intel ID and local 0..3",
        nvrm_fb_logical_head(NVRM_FB_GOP, 1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u) == -1 &&
        nvrm_fb_logical_head(NVRM_FB_GOP, 1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 4u) == -1 &&
        nvrm_fb_logical_head(NVRM_FB_GOP, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u) == -1 &&
        nvrm_fb_logical_head(NVRM_FB_GOP, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 4u) == 4);
    return fails ? 1 : 0;
}
"""
    with tempfile.TemporaryDirectory() as td:
        c_file = Path(td) / "route_check.c"
        exe = Path(td) / "route_check"
        c_file.write_text(src, encoding="utf-8")
        res = subprocess.run(
            [cc, "-O1", "-Wall", "-Werror",
             f"-I{ROOT / 'kexts/NVRM'}", f"-I{ROOT / 'kexts/NVRM/accel'}",
             str(c_file), "-o", str(exe)],
            capture_output=True, text=True,
        )
        arm("production topology routing C fixture compiles cleanly", res.returncode == 0)
        if res.returncode == 0:
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            arm("production topology routing assertions pass (disjoint Intel/NVIDIA controllers, no shim)",
                run.returncode == 0)
        else:
            print(res.stderr)




run_compiled_routing_tests()


print(f"{len(fails)} failed")
sys.exit(1 if fails else 0)
