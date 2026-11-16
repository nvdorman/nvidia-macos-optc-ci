/* Executes the production converter/span checks, including the final-target dataflow. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvaccel_frame_transfer.h"

static void passed(const char *id) { printf("OPTC_CASE {\"id\":\"%s\",\"result\":\"PASS\"}\n", id); }
int main(void)
{
    const size_t size = 16384000;
    uint32_t *source = malloc(size), *target = malloc(size + 8), *panel = malloc(size);
    assert(source && target && panel);
    for (size_t i = 0; i < size / 4; ++i) source[i] = 0xff000000u | (uint32_t)i;
    struct nv_frame_span src = { source, size, 2560, 1600, 10240, NV_IOP_FMT_BGRA };
    struct nv_frame_span dst = { target + 1, size, 2560, 1600, 10240, NV_IOP_FMT_BGRA };
    struct nv_frame_span sink = { panel, size, 2560, 1600, 10240, NV_IOP_FMT_BGRA };
    target[0] = 0x12345678; target[size / 4 + 1] = 0x89abcdef;
    assert(nv_frame_populate(&src, &dst) == NV_FRAME_OK);
    memset(source, 0xa5, size);
    nv_frame_store_drain();
    assert(nv_frame_populate(&dst, &sink) == NV_FRAME_OK);
    assert(!memcmp(dst.address, panel, size));
    assert(memcmp(source, panel, size));
    passed("final_target_dataflow");
    memset((void *)dst.address, 0x5a, size);
    dst.capacity = size - 1;
    assert(nv_frame_populate(&src, &dst) == NV_FRAME_BOUNDS);
    for (size_t i = 1; i <= size / 4; ++i) assert(target[i] == 0x5a5a5a5a);
    dst.capacity = size;
    struct nv_frame_span bad = src;
    bad.pitch = 10236; assert(nv_frame_populate(&bad, &dst) == NV_FRAME_BOUNDS);
    bad = src; bad.height = UINT32_MAX; bad.pitch = UINT32_MAX - 3; bad.capacity = UINT64_MAX;
    assert(nv_frame_populate(&bad, &dst) != NV_FRAME_OK);
    bad = src; bad.address = (void *)(UINTPTR_MAX - 3); assert(nv_frame_validate(&bad) == NV_FRAME_BOUNDS);
    bad = src; bad.format = 0; assert(nv_frame_populate(&bad, &dst) == NV_FRAME_FORMAT);
    assert(target[0] == 0x12345678 && target[size / 4 + 1] == 0x89abcdef);
    passed("copy_bounds");
    uint32_t pixel = 0xfff00000, converted = 0;
    struct nv_frame_span one = { &pixel, 4, 1, 1, 4, NV_IOP_FMT_L10R };
    struct nv_frame_span out = { &converted, 4, 1, 1, 4, NV_IOP_FMT_BGRA };
    assert(nv_frame_populate(&one, &out) == NV_FRAME_OK && converted == 0xffff0000);
    union { uint32_t align[2]; uint16_t half[4]; } half = { .half = {0x3c00, 0, 0, 0x3c00} };
    one.address = &half; one.capacity = 8; one.pitch = 8; one.format = NV_IOP_FMT_RGHA;
    assert(nv_frame_populate(&one, &out) == NV_FRAME_OK && converted == 0xffff0000);
    passed("pixel_formats");
    memset(source, 0, size);
    assert(nv_frame_populate(&src, &dst) == NV_FRAME_OK);
    assert(nv_frame_populate(&dst, &sink) == NV_FRAME_OK);
    for (size_t i = 0; i < size / 4; ++i) assert(panel[i] == 0);
    passed("black_frame");
    free(source); free(target); free(panel);
    return 0;
}
