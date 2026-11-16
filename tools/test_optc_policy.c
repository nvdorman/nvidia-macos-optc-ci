/* Host unit tests for Option C NMIntelFB + NVAccel + NVRM topology/policy helpers.
 *   cc -O1 -Wall -Werror -I./kexts/NMIntelFB -I./kexts/NVRM/accel -I./kexts/NVRM \
 *     tools/test_optc_policy.c -o /tmp/t && /tmp/t
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "intelfb_console.h"
#include "nvaccel_optc_policy.h"
#include "nvrm_fb_topology.h"
#include "nvrm_startup_policy.h"

static int fails = 0;
#define CHK(name, cond) do { if (cond) printf("ok   %s\n", name); else { printf("FAIL %s\n", name); fails++; } } while (0)

struct fb_stub {
    enum nvrm_fb_kind kind;
    int has_id;
    uint64_t dep_id;
    int has_idx;
    uint32_t local_idx;
    uint64_t reg_id;
    int inactive;
    int retains;
};

struct accel_stub {
    struct fb_stub *slots[NVRM_FB_MAX_HEADS];
    struct fb_stub *primary_fb;
    unsigned published_mask;
    unsigned registered_mask;
    unsigned piped_mask;
    struct fb_stub *pipe_fb[16];
    uint32_t pipe_count;
    int has_assoc;
    uint64_t assoc_reg_id;
    uint32_t assoc_dense_idx;
};

static int stub_fb_published(struct accel_stub *a, struct fb_stub *fb)
{
    int logical = nvrm_fb_logical_head(
        fb->kind, fb->has_id, fb->dep_id, fb->has_idx, fb->local_idx);
    if (logical < 0 || (unsigned)logical >= NVRM_FB_MAX_HEADS)
        return -1;
    unsigned slot = (unsigned)logical;
    enum nm_optc_claim_outcome outcome = nm_optc_decide_claim(a->slots[slot], fb);
    if (outcome == NM_OPTC_CLAIM_ACQUIRED) {
        fb->retains++;
        a->slots[slot] = fb;
        a->published_mask |= (1u << slot);
    } else if (outcome != NM_OPTC_CLAIM_IDEMPOTENT) {
        return 0;
    }
    if (nm_optc_should_set_primary_fb(fb->kind, logical) &&
        a->slots[0] == fb && a->primary_fb == NULL) {
        fb->retains++;
        a->primary_fb = fb;
    }
    return 1;
}

static void stub_sync_head4_assoc(struct accel_stub *a)
{
    struct fb_stub *fb4 = a->slots[NVRM_FB_LOGICAL_HYBRID_HEAD];
    if (!fb4 || !(a->piped_mask & NVRM_FB_HYBRID_PUBLISHED_BIT)) {
        a->has_assoc = 0;
        a->assoc_reg_id = 0;
        a->assoc_dense_idx = 0;
        return;
    }
    unsigned matches = 0;
    uint32_t found = 0;
    for (uint32_t d = 0; d < a->pipe_count && d < 16u; d++) {
        if (a->pipe_fb[d] == fb4) {
            found = d;
            matches++;
        }
    }
    if (matches == 1 &&
        nm_optc_head4_assoc_valid(fb4, a->pipe_fb[found], found, a->pipe_count, fb4->inactive) &&
        nvrm_fb_logical_head(fb4->kind, fb4->has_id, fb4->dep_id, fb4->has_idx, fb4->local_idx) == NVRM_FB_LOGICAL_HYBRID_HEAD &&
        fb4->reg_id != 0) {
        a->has_assoc = 1;
        a->assoc_reg_id = fb4->reg_id;
        a->assoc_dense_idx = found;
    } else {
        a->has_assoc = 0;
        a->assoc_reg_id = 0;
        a->assoc_dense_idx = 0;
    }
}

static void stub_register_pending(struct accel_stub *a, int igpu_enabled)
{
    for (unsigned i = 0; i < NVRM_FB_MAX_HEADS; i++) {
        struct fb_stub *fb = a->slots[i];
        if (!fb) {
            if (igpu_enabled && i < 4u && ((a->published_mask >> 4) & 1u)) {
                i = 3u;
                continue;
            }
            if (a->published_mask >> i)
                break;
            break;
        }
        if (a->registered_mask & (1u << i))
            continue;
        if (i == 4u && !igpu_enabled)
            break;
        uint32_t b = a->pipe_count;
        a->registered_mask |= (1u << i);
        if (b < 16u) {
            a->pipe_fb[b] = fb;
            a->pipe_count = b + 1u;
            a->piped_mask |= (1u << i);
        }
    }
    stub_sync_head4_assoc(a);
}

int main(void)
{
    uint64_t phys = 0, bytes = 0;
    const int sentinel = 1;

    /* NMIntelFB console fallback checks (2560x1600 @ 0x70800000, pitch 10240) */
    CHK("valid Insyde GOP console accepted",
        nm_intelfb_console_ok(0x70800000ULL, 2560, 1600, 10240, 32, 2560, 1600, 10240, &phys, &bytes) == 1 &&
        phys == 0x70800000ULL && bytes == 16384000ULL);
    CHK("PE_Video low flag bits masked before page check",
        nm_intelfb_console_ok(0x70800003ULL, 2560, 1600, 10240, 32, 2560, 1600, 10240, &phys, &bytes) == 1 &&
        phys == 0x70800000ULL);
    CHK("zero baseAddr rejected",
        nm_intelfb_console_ok(0ULL, 2560, 1600, 10240, 32, 2560, 1600, 10240, &phys, &bytes) == 0);
    CHK("non-page-aligned baseAddr rejected",
        nm_intelfb_console_ok(0x70800400ULL, 2560, 1600, 10240, 32, 2560, 1600, 10240, &phys, &bytes) == 0);
    CHK("wrong console depth rejected",
        nm_intelfb_console_ok(0x70800000ULL, 2560, 1600, 10240, 16, 2560, 1600, 10240, &phys, &bytes) == 0);
    CHK("mismatched console width/height rejected",
        nm_intelfb_console_ok(0x70800000ULL, 1920, 1080, 10240, 32, 2560, 1600, 10240, &phys, &bytes) == 0);
    CHK("mismatched row stride rejected",
        nm_intelfb_console_ok(0x70800000ULL, 2560, 1600, 10496, 32, 2560, 1600, 10240, &phys, &bytes) == 0);

    /* NVAccel Option C policy checks */
    CHK("auto iGPU off when nmhybrid=0", nm_optc_should_enable_igpu(NMH_OFF, 0) == 0);
    CHK("auto iGPU on when nmhybrid=1", nm_optc_should_enable_igpu(NMH_COPY, 0) == 1);
    CHK("auto iGPU disabled by -nvaccelnoigpu", nm_optc_should_enable_igpu(NMH_COPY, 1) == 0);

    CHK("headless with 0 NVIDIA heads and nmhybrid=0", nm_optc_has_active_head(0, NMH_OFF, 1, 1) == 0);
    CHK("headless when Intel FB not published", nm_optc_has_active_head(0, NMH_COPY, 1, 0) == 0);
    CHK("headless when gNvIgpu=0", nm_optc_has_active_head(0, NMH_COPY, 0, 1) == 0);
    CHK("active head via NMIntelFB when 0 NVIDIA heads", nm_optc_has_active_head(0, NMH_COPY, 1, 1) == 1);
    CHK("active head via NVIDIA FB always", nm_optc_has_active_head(1, NMH_OFF, 0, 0) == 1);

    CHK("CPU-only offscreen BAR1 backing off at nmhybrid=0",
        nm_optc_use_offscreen_fb_resource(NMH_OFF, &sentinel, 0ULL, 0) == 0);
    CHK("CPU-only offscreen BAR1 backing off for NVIDIA FB",
        nm_optc_use_offscreen_fb_resource(NMH_COPY, &sentinel, 0ULL, 1) == 0);
    CHK("CPU-only offscreen BAR1 backing off if kva null",
        nm_optc_use_offscreen_fb_resource(NMH_COPY, NULL, 0ULL, 0) == 0);
    CHK("CPU-only offscreen BAR1 backing ON for NMIntelFB at nmhybrid>=1",
        nm_optc_use_offscreen_fb_resource(NMH_COPY, &sentinel, 0ULL, 0) == 1);

    CHK("scanout backing falls back to pitch*height when actualSize==0",
        nm_optc_scanout_backing_bytes(0ULL, 10240U, 1600U) == 16384000ULL);
    CHK("scanout backing page-aligns non-multiple sizes",
        nm_optc_scanout_backing_bytes(0ULL, 7684U, 1080U) == ((7684ULL * 1080ULL + 0xFFFULL) & ~0xFFFULL));
    CHK("scanout backing rejects empty geometry",
        nm_optc_scanout_backing_bytes(0ULL, 0U, 1600U) == 0ULL);

    CHK("8 GiB Resizable BAR budget is half BAR1",
        nm_optc_vram_budget(8ULL << 30) == (4ULL << 30));
    CHK("256 MiB BAR1 falls back to 192 MiB budget",
        nm_optc_vram_budget(256ULL << 20) == NVRM_VRAM_BAR1_BUDGET);
    CHK("64 KiB alignment matches NVRMFB",
        nm_optc_align_vram_size(16384000ULL) == 16384000ULL &&
        nm_optc_align_vram_size(4096ULL) == 65536ULL &&
        nm_optc_align_vram_size(0ULL) == 0ULL);

    /* Case 1: Intel-only (local 0 on Intel controller -> logical 4; primary empty; dense 0) */
    {
        struct accel_stub a;
        memset(&a, 0, sizeof a);
        struct fb_stub intel = { NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL, 1, 0u, 0x1004ULL, 0, 0 };
        CHK("case 1a: Intel local0 + Intel ID classifies to logical 4",
            nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL, 1, 0u) == 4);
        CHK("case 1b: Intel-only claims slot 4, primary stays NULL, no premature association",
            stub_fb_published(&a, &intel) == 1 &&
            a.slots[4] == &intel && a.slots[0] == NULL && a.primary_fb == NULL &&
            a.published_mask == (1u << 4) && a.has_assoc == 0 &&
            nm_optc_has_active_head(a.primary_fb != NULL, NMH_COPY, 1, a.slots[4] != NULL) == 1);
        stub_register_pending(&a, 1);
        CHK("case 1c: first registered Intel panel gets dense pipe 0 distinct from logical 4",
            a.has_assoc == 1 && a.assoc_reg_id == 0x1004ULL && a.assoc_dense_idx == 0u &&
            a.pipe_fb[0] == &intel && (a.piped_mask == (1u << 4)));
    }

    /* Case 2: NVIDIA 0 then Intel, and Intel then NVIDIA 0 */
    {
        struct accel_stub a;
        memset(&a, 0, sizeof a);
        struct fb_stub nv0   = { NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u, 0x1000ULL, 0, 0 };
        struct fb_stub intel = { NVRM_FB_INTEL,  1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u, 0x1004ULL, 0, 0 };
        CHK("case 2a: NVIDIA0 then Intel keeps distinct local0 slots and single fFB=nv0",
            stub_fb_published(&a, &nv0) == 1 && stub_fb_published(&a, &intel) == 1 &&
            a.slots[0] == &nv0 && a.slots[4] == &intel && a.primary_fb == &nv0);
        stub_register_pending(&a, 1);
        CHK("case 2b: NVIDIA0 then Intel assigns dense 0 to NVIDIA0, dense 1 to Intel logical 4",
            a.pipe_fb[0] == &nv0 && a.pipe_fb[1] == &intel &&
            a.has_assoc == 1 && a.assoc_dense_idx == 1u && a.assoc_reg_id == 0x1004ULL);

        struct accel_stub b;
        memset(&b, 0, sizeof b);
        struct fb_stub intel_b = { NVRM_FB_INTEL,  1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u, 0x2004ULL, 0, 0 };
        struct fb_stub nv0_b   = { NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u, 0x2000ULL, 0, 0 };
        CHK("case 2c: Intel first stays in slot 4 with primary NULL until NVIDIA0 arrives",
            stub_fb_published(&b, &intel_b) == 1 &&
            b.slots[4] == &intel_b && b.slots[0] == NULL && b.primary_fb == NULL);
        stub_register_pending(&b, 1);
        CHK("case 2d: Intel registered first gets dense 0 before NVIDIA0 publishes",
            b.has_assoc == 1 && b.assoc_dense_idx == 0u && b.assoc_reg_id == 0x2004ULL);
        CHK("case 2e: late NVIDIA0 claims slot 0 and primary without displacing Intel slot 4",
            stub_fb_published(&b, &nv0_b) == 1 &&
            b.slots[0] == &nv0_b && b.primary_fb == &nv0_b && b.slots[4] == &intel_b);
        stub_register_pending(&b, 1);
        CHK("case 2f: late NVIDIA0 gets dense 1 while Intel stays dense 0 / logical 4",
            b.pipe_fb[0] == &intel_b && b.pipe_fb[1] == &nv0_b &&
            b.has_assoc == 1 && b.assoc_dense_idx == 0u && b.assoc_reg_id == 0x2004ULL);
    }

    /* Case 3: NVIDIA slot 1/2/3 without slot 0 must never turn Intel into NVIDIA allocation provider */
    {
        struct accel_stub a;
        memset(&a, 0, sizeof a);
        struct fb_stub nv1   = { NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 1u, 0x3001ULL, 0, 0 };
        struct fb_stub nv2   = { NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 2u, 0x3002ULL, 0, 0 };
        struct fb_stub nv3   = { NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 3u, 0x3003ULL, 0, 0 };
        struct fb_stub intel = { NVRM_FB_INTEL,  1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u, 0x3004ULL, 0, 0 };
        CHK("case 3a: NVIDIA 1..3 populate slots 1..3 while primary fFB remains NULL",
            stub_fb_published(&a, &nv1) == 1 && stub_fb_published(&a, &nv2) == 1 && stub_fb_published(&a, &nv3) == 1 &&
            a.slots[0] == NULL && a.primary_fb == NULL &&
            nm_optc_has_active_head(a.primary_fb != NULL, NMH_COPY, 1, a.slots[4] != NULL) == 0);
        CHK("case 3b: Intel still enters slot 4, never slot 0 or fFB, keeping offscreen BAR1 path",
            stub_fb_published(&a, &intel) == 1 &&
            a.slots[4] == &intel && a.slots[0] == NULL && a.primary_fb == NULL &&
            nm_optc_should_set_primary_fb(NVRM_FB_INTEL, 4) == 0 &&
            nm_optc_use_offscreen_fb_resource(NMH_COPY, &sentinel, 0ULL, 0) == 1);
    }

    /* Case 4: Missing props, out-of-range NVIDIA index, legacy Intel index 4, wrong/colliding ID, unknown class */
    {
        CHK("case 4a: missing controller ID or local index rejected (never defaults to 0)",
            nvrm_fb_logical_head(NVRM_FB_NVIDIA, 0, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 0, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_INTEL,  0, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_INTEL,  1, NVRM_FB_CONTROLLER_ID_INTEL,  0, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_GOP,    0, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 4u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_GOP,    1, NVRM_FB_CONTROLLER_ID_NVIDIA, 0, 4u) == -1);
        CHK("case 4b: NVIDIA index >= 4 rejected",
            nvrm_fb_logical_head(NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 4u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 5u) == -1);
        CHK("case 4c: Intel legacy local index 4 and shared NVIDIA ID rejected (no shim)",
            nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 4u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 4u) == -1);
        CHK("case 4d: ID collision / wrong ID / foreign class rejected",
            nvrm_fb_logical_head(NVRM_FB_NVIDIA,  1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_NVIDIA,  1, 0x12345678ULL,                1, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_INTEL,   1, 0x12345678ULL,                1, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_UNKNOWN, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_UNKNOWN, 1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u) == -1);
        CHK("case 4e: GOP legacy routing preserved ONLY for GOP with NVIDIA ID + local 4",
            nvrm_fb_logical_head(NVRM_FB_GOP, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 4u) == 4 &&
            nvrm_fb_logical_head(NVRM_FB_GOP, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_GOP, 1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u) == -1 &&
            nvrm_fb_logical_head(NVRM_FB_GOP, 1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 4u) == -1);
    }

    /* Case 5: Idempotent duplicate pointer claim & fail-closed rejection of conflicting claimant */
    {
        struct accel_stub a;
        memset(&a, 0, sizeof a);
        struct fb_stub nv0      = { NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u, 0x5000ULL, 0, 0 };
        struct fb_stub nv0_dup  = { NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u, 0x5001ULL, 0, 0 };
        struct fb_stub intel    = { NVRM_FB_INTEL,  1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u, 0x5004ULL, 0, 0 };
        struct fb_stub gop_conf = { NVRM_FB_GOP,    1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 4u, 0x5005ULL, 0, 0 };
        CHK("case 5a: initial claim succeeds with expected retain counts",
            stub_fb_published(&a, &nv0) == 1 && stub_fb_published(&a, &intel) == 1 &&
            nv0.retains == 2 && intel.retains == 1 && a.published_mask == 0x11u);
        CHK("case 5b: duplicate same-pointer notification is idempotent with no retain/mask duplication",
            stub_fb_published(&a, &nv0) == 1 && stub_fb_published(&a, &intel) == 1 &&
            nv0.retains == 2 && intel.retains == 1 && a.published_mask == 0x11u);
        CHK("case 5c: conflicting claimant on slot 0 or 4 rejected without changing owner, mask, fFB, or retains",
            stub_fb_published(&a, &nv0_dup) == 0 && stub_fb_published(&a, &gop_conf) == 0 &&
            a.slots[0] == &nv0 && a.primary_fb == &nv0 && a.slots[4] == &intel &&
            nv0_dup.retains == 0 && gop_conf.retains == 0 && a.published_mask == 0x11u);
    }

    /* Case 6: Logical Head 4 mask remains 16 (0x10) and auto-go/release policy unchanged */
    {
        int logical4 = nvrm_fb_logical_head(NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL, 1, 0u);
        unsigned mask = (logical4 >= 0) ? (1u << (unsigned)logical4) : 0u;
        CHK("case 6: logical head 4 bitmask is 16 (0x10) and auto-go release policy intact",
            logical4 == 4 && mask == 16u && mask == NVRM_FB_HYBRID_PUBLISHED_BIT &&
            nvrmReleaseBeforeArming(0u, true) == true &&
            nvrmReleaseBeforeArming(mask, true) == false &&
            nvrmHeadlessDeadlineSec(60u, 10000u) == 70u);
    }

    /* Case 7: Foreign Intel using new Intel controller ID counted lit by power policy; unverified/failed enum fail-closed */
    {
        enum nvrm_fb_rtd3_role intel_ok = nvrm_fb_classify_for_rtd3(
            NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL, 1, 1, 0u, 1);
        enum nvrm_fb_rtd3_role intel_bad_id = nvrm_fb_classify_for_rtd3(
            NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 1, 0u, 1);
        enum nvrm_fb_rtd3_role intel_bad_idx = nvrm_fb_classify_for_rtd3(
            NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL, 1, 1, 4u, 1);
        enum nvrm_fb_rtd3_role intel_missing = nvrm_fb_classify_for_rtd3(
            NVRM_FB_INTEL, 0, 0ULL, 0, 0, 0u, 0);
        CHK("case 7a: verified Intel classifies as foreign; hybrid on -> lit, hybrid off -> not lit",
            intel_ok == NVRM_RTD3_FB_FOREIGN &&
            nvrm_rtd3_evaluate_heads(1, 0, 1, 0, 1) == 1 &&
            nvrm_rtd3_evaluate_heads(1, 0, 1, 0, 0) == 0);
        CHK("case 7b: unverified Intel or enumeration failure fails closed as lit",
            intel_bad_id == NVRM_RTD3_FB_UNVERIFIED &&
            intel_bad_idx == NVRM_RTD3_FB_UNVERIFIED &&
            intel_missing == NVRM_RTD3_FB_UNVERIFIED &&
            nvrm_rtd3_evaluate_heads(1, 0, 0, 1, 0) >= 1 &&
            nvrm_rtd3_evaluate_heads(0, 0, 0, 0, 0) >= 1);
    }

    /* Case 8: Association properties never appear before verified pipe match and are removed on mismatch/stop */
    {
        struct accel_stub a;
        memset(&a, 0, sizeof a);
        struct fb_stub intel = { NVRM_FB_INTEL,  1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 0u, 0x8004ULL, 0, 0 };
        struct fb_stub other = { NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0u, 0x8000ULL, 0, 0 };
        stub_fb_published(&a, &intel);
        stub_sync_head4_assoc(&a);
        CHK("case 8a: no association metadata published before registered pipe match",
            a.has_assoc == 0 && a.assoc_reg_id == 0ULL);
        stub_register_pending(&a, 1);
        CHK("case 8b: association published once pipe pointer matches fFBs[4]",
            a.has_assoc == 1 && a.assoc_reg_id == 0x8004ULL && a.assoc_dense_idx == 0u);
        a.pipe_fb[0] = &other;
        stub_sync_head4_assoc(&a);
        CHK("case 8c: pipe mismatch clears association metadata (no stale success)",
            a.has_assoc == 0 && a.assoc_reg_id == 0ULL);
        a.pipe_fb[0] = &intel;
        stub_sync_head4_assoc(&a);
        CHK("case 8d: valid match restores association, inactive/stop clears it",
            a.has_assoc == 1 && (intel.inactive = 1, stub_sync_head4_assoc(&a), a.has_assoc == 0 && a.assoc_reg_id == 0ULL));
    }

    printf("%d failed\n", fails);
    return fails ? 1 : 0;
}
