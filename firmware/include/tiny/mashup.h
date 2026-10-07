/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Mashups (docs/spec/firmware.md "Playlist" Mashups): several plugins on one screen. The split tree of a
 * config `playlist` entry `{ kind: "mashup", split, members }` as a flat node array, and the boxes it makes. */
#ifndef TINY_MASHUP_H
#define TINY_MASHUP_H

#include <stdbool.h>
#include <stdint.h>

#define MASHUP_SCENE_MINOR 19 /* the scene minor a mashup needs; no bump here: the bump is its own last commit (AGENTS.md) */
#define MASHUP_MEMBERS_MIN 2
#define MASHUP_MEMBERS_MAX 4
#define MASHUP_NODES_MAX (2 * MASHUP_MEMBERS_MAX - 1) /* leaves + internal nodes of a full binary tree */
#define MASHUP_PER_PLAYLIST 8
#define MASHUP_SEP_PX 1 /* host-drawn line between boxes, taken from the first box of a split */

typedef enum { MASHUP_LEAF = 0, MASHUP_H = 1, MASHUP_V = 2 } mashup_kind_t;

/* A node: a leaf (member) or an internal split. `ratio` in permille 1..999 = round(wire ratio * 1000): the share of the
 * space for `left` (h: the left part, v: the top part). `left`, `right`: node indexes (root = 0, children after parents) */
typedef struct {
    uint8_t kind;   /* mashup_kind_t */
    uint8_t member; /* leaf: 0..3 */
    uint16_t ratio; /* internal: permille */
    uint8_t left, right;
} mashup_node_t;

typedef struct {
    uint8_t n;
    mashup_node_t node[MASHUP_NODES_MAX];
} mashup_split_t;

typedef struct {
    int32_t x, y, w, h;
} tiny_box_t;

/* Structure valid: nodes reachable from the root exactly once, 2..4 leaves, members 0..n-1 each exactly once, ratios
 * 1..999. `members` = the entry's member count (2..4). */
bool mashup_split_valid(const mashup_split_t *s, int members);

/* The box of every member (out[member]) on a w x h screen. Depth first from (0, 0, w, h); an internal node gives
 * first = clamp(round(size * ratio / 1000), 1, size - 1) (round half up, integer math) to `left`, the rest to `right`,
 * so the boxes tile the screen without gaps. Returns false for an invalid tree or a screen under 1 px per split. */
bool mashup_boxes(const mashup_split_t *s, int members, int32_t w, int32_t h, tiny_box_t out[MASHUP_MEMBERS_MAX]);

#endif
