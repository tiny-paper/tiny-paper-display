/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sim_json.h"

#include "tiny/cbor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *err;
} ps_t;

static void ws(ps_t *s)
{
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')
        s->p++;
}

static sj_t *node(sj_type_t t)
{
    sj_t *n = calloc(1, sizeof *n);
    n->type = t;
    return n;
}

static void put_utf8(char **o, unsigned cp)
{
    char *q = *o;
    if (cp < 0x80) {
        *q++ = (char)cp;
    } else if (cp < 0x800) {
        *q++ = (char)(0xC0 | (cp >> 6));
        *q++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *q++ = (char)(0xE0 | (cp >> 12));
        *q++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *q++ = (char)(0x80 | (cp & 0x3F));
    }
    *o = q;
}

static char *pstr(ps_t *s)
{
    if (*s->p != '"') {
        s->err = "expected string";
        return NULL;
    }
    s->p++;
    const char *st = s->p;
    size_t n = 0;
    while (st[n] && st[n] != '"') {
        if (st[n] == '\\' && st[n + 1])
            n++;
        n++;
    }
    char *out = malloc(n + 1), *o = out;
    while (*s->p && *s->p != '"') {
        if (*s->p == '\\') {
            s->p++;
            char c = *s->p++;
            switch (c) {
            case 'n': *o++ = '\n'; break;
            case 't': *o++ = '\t'; break;
            case 'r': *o++ = '\r'; break;
            case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4 && *s->p; i++) {
                    char h = *s->p++;
                    cp = cp * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                put_utf8(&o, cp);
                break;
            }
            default: *o++ = c; break;
            }
        } else {
            *o++ = *s->p++;
        }
    }
    *o = 0;
    if (*s->p != '"') {
        s->err = "unterminated string";
        free(out);
        return NULL;
    }
    s->p++;
    return out;
}

static sj_t *pval(ps_t *s, int depth)
{
    ws(s);
    if (depth > 32) {
        s->err = "too deep";
        return NULL;
    }
    char c = *s->p;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        sj_t *n = node(obj ? SJ_OBJ : SJ_ARR), **tail = &n->child;
        s->p++;
        ws(s);
        if (*s->p == (obj ? '}' : ']')) {
            s->p++;
            return n;
        }
        for (;;) {
            char *key = NULL;
            ws(s);
            if (obj) {
                key = pstr(s);
                if (!key)
                    goto fail;
                ws(s);
                if (*s->p != ':') {
                    free(key);
                    s->err = "expected ':'";
                    goto fail;
                }
                s->p++;
            }
            sj_t *v = pval(s, depth + 1);
            if (!v) {
                free(key);
                goto fail;
            }
            v->key = key;
            *tail = v;
            tail = &v->next;
            n->count++;
            ws(s);
            if (*s->p == ',') {
                s->p++;
                continue;
            }
            if (*s->p == (obj ? '}' : ']')) {
                s->p++;
                return n;
            }
            s->err = "expected ',' or end";
        fail:
            sj_free(n);
            return NULL;
        }
    }
    if (c == '"') {
        char *str = pstr(s);
        if (!str)
            return NULL;
        sj_t *n = node(SJ_STR);
        n->str = str;
        return n;
    }
    if (!strncmp(s->p, "true", 4) || !strncmp(s->p, "false", 5)) {
        sj_t *n = node(SJ_BOOL);
        n->b = s->p[0] == 't';
        s->p += n->b ? 4 : 5;
        return n;
    }
    if (!strncmp(s->p, "null", 4)) {
        s->p += 4;
        return node(SJ_NULL);
    }
    char *end;
    double d = strtod(s->p, &end);
    if (end == s->p) {
        s->err = "unexpected character";
        return NULL;
    }
    s->p = end;
    sj_t *n = node(SJ_NUM);
    n->num = d;
    return n;
}

sj_t *sj_parse(const char *text, char *err, size_t err_cap)
{
    ps_t s = {text, NULL};
    sj_t *v = pval(&s, 0);
    if (v) {
        ws(&s);
        if (*s.p) {
            s.err = "trailing data";
            sj_free(v);
            v = NULL;
        }
    }
    if (!v && err)
        snprintf(err, err_cap, "%s at offset %ld", s.err ? s.err : "error", (long)(s.p - text));
    return v;
}

void sj_free(sj_t *v)
{
    while (v) {
        sj_t *nx = v->next;
        sj_free(v->child);
        free(v->str);
        free(v->key);
        free(v);
        v = nx;
    }
}

const sj_t *sj_get(const sj_t *o, const char *key)
{
    if (!o || o->type != SJ_OBJ)
        return NULL;
    for (const sj_t *c = o->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key))
            return c;
    return NULL;
}

const sj_t *sj_path(const sj_t *root, const char *dotted)
{
    char buf[128];
    snprintf(buf, sizeof buf, "%s", dotted);
    const sj_t *v = root;
    for (char *tok = strtok(buf, "."); tok && v; tok = strtok(NULL, "."))
        v = sj_get(v, tok);
    return v;
}

double sj_num(const sj_t *v, double def) { return v && v->type == SJ_NUM ? v->num : (v && v->type == SJ_BOOL ? v->b : def); }
const char *sj_str(const sj_t *v, const char *def) { return v && v->type == SJ_STR ? v->str : def; }
bool sj_bool(const sj_t *v, bool def) { return v && v->type == SJ_BOOL ? v->b : (v && v->type == SJ_NUM ? v->num != 0 : def); }
size_t sj_len(const sj_t *v) { return v && (v->type == SJ_ARR || v->type == SJ_OBJ) ? v->count : 0; }

const sj_t *sj_at(const sj_t *a, size_t i)
{
    if (!a || (a->type != SJ_ARR && a->type != SJ_OBJ))
        return NULL;
    const sj_t *c = a->child;
    while (c && i--)
        c = c->next;
    return c;
}

/* JSON value -> CBOR item (integers stay integers) */
static void to_cbor(cbor_w_t *w, const sj_t *v)
{
    switch (v->type) {
    case SJ_NULL: cbor_put_null(w); break;
    case SJ_BOOL: cbor_put_bool(w, v->b); break;
    case SJ_NUM:
        if (v->num == (double)(long long)v->num)
            cbor_put_int(w, (int64_t)v->num);
        else
            cbor_put_f64(w, v->num);
        break;
    case SJ_STR: cbor_put_str(w, v->str); break;
    case SJ_ARR:
        cbor_put_array(w, v->count);
        for (const sj_t *c = v->child; c; c = c->next)
            to_cbor(w, c);
        break;
    case SJ_OBJ:
        cbor_put_map(w, v->count);
        for (const sj_t *c = v->child; c; c = c->next) {
            cbor_put_str(w, c->key);
            to_cbor(w, c);
        }
        break;
    }
}

size_t sj_to_cbor(const sj_t *v, uint8_t *out, size_t cap)
{
    cbor_w_t w;
    cbor_w_init(&w, out, cap);
    to_cbor(&w, v);
    return w.err ? 0 : w.len;
}
