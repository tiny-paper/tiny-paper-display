/* Tests of the Bluetooth key: docs/spec/vectors/bt-key.json loaded (also used by components/usbdisp's tests).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdio.h>
#include <string.h>

#include "sim_json.h"
#include "unity.h"

static inline size_t bv_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s) / 2;
    TEST_ASSERT_TRUE(n <= cap);
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        TEST_ASSERT_EQUAL_INT(1, sscanf(s + 2 * i, "%2x", &v));
        out[i] = (uint8_t)v;
    }
    return n;
}

/* the field `key` of obj as bytes; its length */
static inline size_t bv_get(const sj_t *obj, const char *key, uint8_t *out, size_t cap)
{
    const char *s = sj_str(sj_get(obj, key), NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, key);
    return bv_hex(s, out, cap);
}

static inline sj_t *bv_load(void)
{
    static char text[1 << 16], err[128];
    FILE *f = fopen(TINY_ROOT "/docs/spec/vectors/bt-key.json", "r");
    TEST_ASSERT_NOT_NULL(f);
    text[fread(text, 1, sizeof text - 1, f)] = 0;
    fclose(f);
    sj_t *v = sj_parse(text, err, sizeof err);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, err);
    return v;
}
