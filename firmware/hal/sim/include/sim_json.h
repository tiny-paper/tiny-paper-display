/* Minimal JSON DOM for simulator configs (malloc based, sim only).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SIM_JSON_H
#define SIM_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { SJ_NULL, SJ_BOOL, SJ_NUM, SJ_STR, SJ_ARR, SJ_OBJ } sj_type_t;

typedef struct sj {
    sj_type_t type;
    double num;
    bool b;
    char *str;            /* SJ_STR value; also the key when the node is an object member */
    char *key;
    struct sj *child, *next;
    size_t count;
} sj_t;

sj_t *sj_parse(const char *text, char *err, size_t err_cap);
void sj_free(sj_t *v);
const sj_t *sj_get(const sj_t *obj, const char *key);      /* NULL if missing or obj not an object */
const sj_t *sj_path(const sj_t *root, const char *dotted);  /* "a.b.c" */
double sj_num(const sj_t *v, double def);
const char *sj_str(const sj_t *v, const char *def);
bool sj_bool(const sj_t *v, bool def);
size_t sj_len(const sj_t *v);
const sj_t *sj_at(const sj_t *arr, size_t i);
/* JSON -> CBOR (integral numbers as integers). 0 if cap is too small. */
size_t sj_to_cbor(const sj_t *v, uint8_t *out, size_t cap);

#endif
