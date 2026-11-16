/* Host unit tests for Option C NMIntelFB + NVAccel policies.
 *   cc -O1 -Wall -Werror -I../kexts/NMIntelFB -I../kexts/NVRM/accel -I../kexts/NVRM test_optc_policy.c -o /tmp/t && /tmp/t
 */
#include <stdio.h>
#include "intelfb_console.h"
#include "nvaccel_optc_policy.h"

static int fails = 0;
#define CHK(name, cond) do { if (cond) printf("ok   %s\n", name); else { printf("FAIL %s\n", name); fails++; } } while (0)

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

    printf("%d failed\n", fails);
    return fails ? 1 : 0;
}
