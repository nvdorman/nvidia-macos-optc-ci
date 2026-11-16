/* Hybrid laptops and idle power-down (kexts/NVRM/nv_rtd3_policy.h + nvrm_fb_topology.h), on the host.
 *   cc -Wall -Werror -I./kexts/NVRM tools/test_hybrid_rtd3.c -o /tmp/t && /tmp/t -> 0 failed
 *   cc -DOLD_RULES ...   (the counts before the hybrid change: no accelerator clients, no foreign heads)  -> failures */
#include <stdio.h>
#include "nv_rtd3_policy.h"
#include "nvrm_fb_topology.h"

static int fails;
#define ARM(name, cond) do { if (cond) printf("ok   %s\n", name); else { printf("FAIL %s\n", name); fails++; } } while (0)
#ifdef OLD_RULES
#define HEADS(o, f, h) (o)
#define CLIENTS(r, a) (r)
#else
#define HEADS(o, f, h) nv_rtd3_heads_lit(o, f, h)
#define CLIENTS(r, a) nv_rtd3_clients_total(r, a)
#endif
static int may(int heads, int clients, unsigned idle) { struct nv_rtd3 s = {1, NV_RTD3_ON, 1}; return nv_rtd3_may_power_off(&s, 1, heads, clients, idle); }

struct rtd3_fb_sample {
    enum nvrm_fb_kind kind;
    int has_id;
    uint64_t dep_id;
    int id_type_ok;
    int has_idx;
    uint32_t local_idx;
    int idx_type_ok;
};

static int eval_fb_list(int enum_ok, const struct rtd3_fb_sample *fbs, unsigned count, int hybrid_level)
{
    int own = 0, foreign = 0, unverified = 0;
    for (unsigned i = 0; i < count; i++) {
        enum nvrm_fb_rtd3_role role = nvrm_fb_classify_for_rtd3(
            fbs[i].kind,
            fbs[i].has_id, fbs[i].dep_id, fbs[i].id_type_ok,
            fbs[i].has_idx, fbs[i].local_idx, fbs[i].idx_type_ok);
        if (role == NVRM_RTD3_FB_OWN) own++;
        else if (role == NVRM_RTD3_FB_FOREIGN) foreign++;
        else if (role == NVRM_RTD3_FB_UNVERIFIED) unverified++;
    }
    return nvrm_rtd3_evaluate_heads(enum_ok, own, foreign, unverified, hybrid_level);
}

int main(void)
{
    ARM("iGPU drives the desktop (switch off), no Metal app: the GPU powers off after 60 s", may(HEADS(0, 1, 0), CLIENTS(0, 0), 60) == 1);
    ARM("iGPU drives the desktop, a Metal app is attached to the accelerator: the GPU stays on", may(HEADS(0, 1, 0), CLIENTS(0, 1), 600) == 0);
    ARM("switch on (the GPU composites for the iGPU panel): never powers off, even with no app", may(HEADS(0, 1, 1), CLIENTS(0, 0), 600) == 0);
    ARM("switch on, level 2: same", may(HEADS(0, 1, 2), CLIENTS(0, 0), 600) == 0);
    ARM("its own display lit: stays on", may(HEADS(1, 0, 0), CLIENTS(0, 0), 600) == 0);
    ARM("a foreign panel with the switch off says nothing about this GPU", nv_rtd3_heads_lit(0, 3, 0) == 0);

    /* Production classification tests (rtd3Heads contract §5.4 / §7.1) */
    {
        const struct rtd3_fb_sample intel_new[] = {
            { NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL, 1, 1, 0u, 1 }
        };
        ARM("NMIntelFB with independent Intel ID + local 0 blocks RTD3 when hybrid is on",
            eval_fb_list(1, intel_new, 1, 1) == 1 && may(eval_fb_list(1, intel_new, 1, 1), 0, 600) == 0);
        ARM("NMIntelFB with independent Intel ID + local 0 allows RTD3 when hybrid is off and no clients",
            eval_fb_list(1, intel_new, 1, 0) == 0 && may(eval_fb_list(1, intel_new, 1, 0), 0, 60) == 1);
    }
    {
        const struct rtd3_fb_sample intel_legacy_or_bad[] = {
            { NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 1, 4u, 1 }
        };
        const struct rtd3_fb_sample intel_missing_props[] = {
            { NVRM_FB_INTEL, 0, 0ULL, 0, 0, 0u, 0 }
        };
        const struct rtd3_fb_sample intel_bad_type[] = {
            { NVRM_FB_INTEL, 1, NVRM_FB_CONTROLLER_ID_INTEL, 0, 1, 0u, 1 }
        };
        ARM("NMIntelFB with legacy shared NVIDIA ID/index 4 fails closed as lit",
            eval_fb_list(1, intel_legacy_or_bad, 1, 0) >= 1 &&
            may(eval_fb_list(1, intel_legacy_or_bad, 1, 0), 0, 600) == 0);
        ARM("NMIntelFB with missing topology properties fails closed as lit",
            eval_fb_list(1, intel_missing_props, 1, 0) >= 1 &&
            may(eval_fb_list(1, intel_missing_props, 1, 0), 0, 600) == 0);
        ARM("NMIntelFB with malformed property type fails closed as lit",
            eval_fb_list(1, intel_bad_type, 1, 0) >= 1);
    }
    {
        const struct rtd3_fb_sample empty[1] = {{ NVRM_FB_UNKNOWN, 0, 0ULL, 0, 0, 0u, 0 }};
        ARM("failed IOFramebuffer enumeration fails closed as lit (never permits power-off)",
            eval_fb_list(0, empty, 0, 0) >= 1 && may(eval_fb_list(0, empty, 0, 0), 0, 600) == 0);
    }
    {
        const struct rtd3_fb_sample mixed[] = {
            { NVRM_FB_UNKNOWN, 0, 0ULL, 1, 0, 0u, 0 },                           /* Apple native FB (unrelated) */
            { NVRM_FB_GOP,     1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 1, 4u, 1 },   /* NMGopFB legacy foreign */
            { NVRM_FB_UNKNOWN, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 0, 0u, 0 }    /* Unchanged foreign provider */
        };
        ARM("Apple unrelated IOFramebuffer ignored; NMGopFB and unchanged foreign providers stay counted",
            eval_fb_list(1, mixed, 1, 1) == 0 &&
            eval_fb_list(1, mixed, 3, 1) == 2 &&
            eval_fb_list(1, mixed, 3, 0) == 0);
    }
    {
        const struct rtd3_fb_sample own_plus_intel[] = {
            { NVRM_FB_NVIDIA, 1, NVRM_FB_CONTROLLER_ID_NVIDIA, 1, 1, 0u, 1 },
            { NVRM_FB_INTEL,  1, NVRM_FB_CONTROLLER_ID_INTEL,  1, 1, 0u, 1 }
        };
        ARM("NVIDIA own head + Intel panel keeps GPU lit even if hybrid switch were 0",
            eval_fb_list(1, own_plus_intel, 2, 0) == 1 && eval_fb_list(1, own_plus_intel, 2, 1) == 2);
    }

    printf("%d failed\n", fails);
    return fails != 0;
}
