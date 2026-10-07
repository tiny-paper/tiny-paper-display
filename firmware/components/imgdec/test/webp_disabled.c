/* WebP-disabled board: refuse at probe and decode, without callbacks, leaks or unbounded reads.
 * Shared fixtures: docs/spec/vectors/webp.json. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgdec.h"
#include "unity.h"

typedef struct {
    uint8_t data[16384];
    size_t n, pos, chunk, live, reads;
} input_t;

static long read_bytes(void *ctx, uint8_t *buf, size_t cap)
{
    input_t *in = ctx;
    size_t n = in->n - in->pos;
    if (n > cap) n = cap;
    if (n > in->chunk) n = in->chunk;
    memcpy(buf, in->data + in->pos, n);
    in->pos += n;
    in->reads++;
    TEST_ASSERT_LESS_THAN(16, in->reads);
    return (long)n;
}

static void *allocate(void *ctx, size_t n)
{
    input_t *in = ctx;
    in->live++;
    return malloc(n);
}

static void release(void *ctx, void *p)
{
    input_t *in = ctx;
    in->live--;
    free(p);
}

static int unexpected_info(void *ctx, const imgdec_info_t *info)
{
    TEST_FAIL_MESSAGE("unsupported WebP reached a decoder callback");
    return 1;
}

static void test_webp_refused(void)
{
    const char *files[] = {"webp-c-tiny-q5.webp", "webp-l-tiny.webp", "webp-t-lossy-ffast.webp"};
    for (size_t k = 0; k < sizeof files / sizeof files[0]; k++) {
        input_t in = {0};
        char path[512];
        snprintf(path, sizeof path, "%s/docs/spec/vectors/images/%s", TINY_ROOT, files[k]);
        FILE *f = fopen(path, "rb");
        TEST_ASSERT_NOT_NULL(f);
        in.n = fread(in.data, 1, sizeof in.data, f);
        TEST_ASSERT_TRUE(feof(f));
        fclose(f);
        imgdec_info_t info;
        TEST_ASSERT_EQUAL(IMGDEC_E_FORMAT, imgdec_probe(in.data, in.n, &info));
        TEST_ASSERT_EQUAL(IMG_WEBP, info.format);
        TEST_ASSERT_EQUAL(k == 2 ? 333 : 97, info.w);
        TEST_ASSERT_EQUAL(k == 2 ? 201 : 61, info.h);
        TEST_ASSERT_EQUAL(info.w, info.rw);
        TEST_ASSERT_EQUAL(info.h, info.rh);
        TEST_ASSERT_EQUAL(0, imgdec_need(&info, 0));
        for (size_t chunk = 1; chunk <= 8192; chunk *= 8192) {
            in.pos = in.reads = 0;
            in.chunk = chunk;
            imgdec_io_t io = {.read = read_bytes, .info = unexpected_info, .alloc = allocate,
                             .free = release, .ctx = &in};
            TEST_ASSERT_EQUAL(IMGDEC_E_FORMAT, imgdec_decode(&io, &info));
            TEST_ASSERT_EQUAL(0, in.live);
        }
    }
}

void setUp(void) {}
void tearDown(void) {}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_webp_refused);
    return UNITY_END();
}
