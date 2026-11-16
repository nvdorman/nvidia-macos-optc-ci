#!/usr/bin/env python3
"""Hybrid panel framebuffer controller-topology routing, start-order, and unwind regression tests.

Replaces literal-comment and historical-git checks with:
1. Compiled C behavioral routing tests using production nvrm_fb_topology.h and nvaccel_optc_policy.h.
2. Call-order and failure-unwind verification across both NMIntelFB::start paths (virtual and physical)
   with negative controls proving misrouted ID/index, map-after-publish, post-start property publication,
   or unchecked setProperty without unwind are rejected.

Does NOT claim NMGopFB's AMD topology has been migrated (NMGopFB only preserves legacy boundary rejection).
"""
import re
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


def extract_function_body(src, sig):
    idx = src.index(sig)
    brace = src.index("{", idx)
    depth = 0
    for pos in range(brace, len(src)):
        ch = src[pos]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[brace:pos + 1]
    raise ValueError(f"Unclosed function {sig}")


def strip_comments(text):
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return text


def check_intelfb_paths(src):
    errors = []
    start_body = strip_comments(extract_function_body(src, "bool NMIntelFB::start(IOService *provider)"))
    if "#ifdef NMINTELFB_VIRTUAL" not in start_body or "#endif" not in start_body:
        return ["missing #ifdef NMINTELFB_VIRTUAL split"]
    a = start_body.index("#ifdef NMINTELFB_VIRTUAL")
    c = start_body.index("#endif", a)
    virt = start_body[a:c]
    phys = start_body[c:]

    # Verify helper or direct property setters use Intel controller ID + local index 0 and check errors
    helper_body = ""
    if "publishDependentIdentity()" in start_body:
        try:
            helper_body = strip_comments(extract_function_body(src, "bool publishDependentIdentity()"))
        except ValueError:
            errors.append("missing publishDependentIdentity() body")

    for label, path_code in (("virtual", virt), ("physical", phys)):
        if "IOFramebuffer::start(provider)" not in path_code:
            errors.append(f"{label}: missing IOFramebuffer::start(provider)")
            continue
        fb_start_pos = path_code.index("IOFramebuffer::start(provider)")
        map_check = path_code.find("if (!fSurfMap)")
        if map_check < 0 or map_check >= fb_start_pos:
            errors.append(f"{label}: surface map check missing or after IOFramebuffer::start")

        # Check identity publication happens after !fSurfMap and before IOFramebuffer::start
        if "publishDependentIdentity()" in path_code:
            pub_pos = path_code.find("publishDependentIdentity()")
            pub_impl = helper_body
            checked_fail = (
                "if (!publishDependentIdentity())" in path_code
                and 'if (!setProperty("IOFBDependentID"' in pub_impl
                and '!setProperty("IOFBDependentIndex"' in pub_impl
            )
        else:
            pub_pos = path_code.find('setProperty("IOFBDependentID"')
            pub_impl = path_code
            checked_fail = (
                'if (!setProperty("IOFBDependentID"' in path_code
                and '!setProperty("IOFBDependentIndex"' in path_code
            )

        if pub_pos < 0 or not (map_check < pub_pos < fb_start_pos):
            errors.append(f"{label}: identity publication not ordered after map check and before IOFramebuffer::start")
        if not checked_fail:
            errors.append(f"{label}: setProperty failure is not checked")
        if "0x4E56524D00000001" in pub_impl or "0x4e56524d00000001" in pub_impl:
            errors.append(f"{label}: stale NVIDIA shared controller ID found in NMIntelFB")
        if re.search(r'setProperty\s*\(\s*"IOFBDependentIndex"\s*,\s*4', pub_impl):
            errors.append(f"{label}: stale local index 4 found in NMIntelFB")
        if ("NVRM_FB_CONTROLLER_ID_INTEL" not in pub_impl and
                "0x4E4D494E00000001" not in pub_impl.upper()):
            errors.append(f"{label}: missing independent Intel controller ID")
        if ("NVRM_FB_INTEL_LOCAL_INDEX" not in pub_impl and
                not re.search(r'setProperty\s*\(\s*"IOFBDependentIndex"\s*,\s*0', pub_impl)):
            errors.append(f"{label}: missing Intel local index 0")
        if 'removeProperty("IOFBDependentID")' not in pub_impl or 'removeProperty("IOFBDependentIndex")' not in pub_impl:
            errors.append(f"{label}: partial property publication is not cleaned up on failure")

    # Physical path: map check and identity publication before thread/service publishers and startUnwind on failure
    if "if (!fSurfMap)" in phys:
        map_pos = phys.index("if (!fSurfMap)")
        for pub in ("resumeCapture();", "tcStart();", "gDmcTarget = this;", "publishBacklight();"):
            p = phys.find(pub)
            if p < 0 or p <= map_pos:
                errors.append(f"physical: {pub} missing or before surface map check")
        if "IOFramebuffer::start(provider)" in phys:
            post_start = phys[phys.index("IOFramebuffer::start(provider)"):]
            if "startUnwind();" not in post_start:
                errors.append("physical: failed IOFramebuffer::start does not invoke startUnwind()")
            if "armVbl();" not in post_start or "registerPowerDriver(" not in post_start:
                errors.append("physical: armVbl/registerPowerDriver missing after IOFramebuffer::start")

    unwind_body = strip_comments(extract_function_body(src, "void NMIntelFB::startUnwind()"))
    for needed in ("removeProperty(\"IOFBDependentID\")", "removeProperty(\"IOFBDependentIndex\")",
                   "OSSafeReleaseNULL(fSurfMap)", "OSSafeReleaseNULL(fMMIO)"):
        if needed not in unwind_body:
            errors.append(f"startUnwind: missing {needed}")
    stop_body = strip_comments(extract_function_body(src, "void NMIntelFB::stop(IOService *provider)"))
    if "startUnwind();" not in stop_body:
        errors.append("stop: does not call startUnwind()")
    return errors


run_compiled_routing_tests()

ifb_src = (ROOT / "kexts/NMIntelFB/intelfb.cpp").read_text(encoding="utf-8")
ifb_errs = check_intelfb_paths(ifb_src)
arm("NMIntelFB::start (virtual + physical) publishes Intel ID + local 0 after map check, checks setProperty, and unwinds cleanly",
    not ifb_errs)
if ifb_errs:
    for e in ifb_errs:
        print("   detail:", e)

nvfb_src = strip_comments(extract_function_body(
    (ROOT / "kexts/NVRMFB/fb/nvrm-fb.cpp").read_text(encoding="utf-8"),
    "bool NVRMNVDAFramebuffer::start(IOService *provider)",
))
arm("NVRMFB publishes NVIDIA dependent ID before super::start",
    nvfb_src.find('setProperty("IOFBDependentID"') != -1 and
    nvfb_src.index('setProperty("IOFBDependentID"') < nvfb_src.index("super::start(provider)"))

# Negative controls on synthetic broken NMIntelFB variants (must be caught by check_intelfb_paths)
neg_after = ifb_src.replace(
    "if (!publishDependentIdentity()) {\n        startUnwind();\n        return false;\n    }", ""
).replace(
    "ifbStep(NMH_STEP_FB_START);",
    "ifbStep(NMH_STEP_FB_START); publishDependentIdentity();"
)
assert neg_after != ifb_src, "neg_after mutation did not match"
arm("negative control 1: post-start identity publication is caught", bool(check_intelfb_paths(neg_after)))

neg_old_id = ifb_src.replace("NVRM_FB_CONTROLLER_ID_INTEL", "0x4E56524D00000001ull")
assert neg_old_id != ifb_src, "neg_old_id mutation did not match"
arm("negative control 2: stale shared NVIDIA controller ID in NMIntelFB is caught", bool(check_intelfb_paths(neg_old_id)))

neg_map_late = ifb_src.replace("if (!fSurfMap) {", "tcStart(); if (!fSurfMap) {")
assert neg_map_late != ifb_src, "neg_map_late mutation did not match"
arm("negative control 3: map check after tcStart is caught", bool(check_intelfb_paths(neg_map_late)))

neg_unchecked = ifb_src.replace(
    "if (!setProperty(\"IOFBDependentID\", NVRM_FB_CONTROLLER_ID_INTEL, 64) ||\n"
    "            !setProperty(\"IOFBDependentIndex\", (unsigned long long)NVRM_FB_INTEL_LOCAL_INDEX, 32)) {",
    "setProperty(\"IOFBDependentID\", NVRM_FB_CONTROLLER_ID_INTEL, 64);\n"
    "        setProperty(\"IOFBDependentIndex\", (unsigned long long)NVRM_FB_INTEL_LOCAL_INDEX, 32);\n        if (false) {"
)
assert neg_unchecked != ifb_src, "neg_unchecked mutation did not match"
arm("negative control 4: unchecked setProperty without failure check is caught", bool(check_intelfb_paths(neg_unchecked)))

print(f"{len(fails)} failed")
sys.exit(1 if fails else 0)
