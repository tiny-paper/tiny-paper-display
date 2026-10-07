/*
 * membudget.h - the one memory budget decision (issue #25, firmware.md "Plugin budgets"). Every memory-heavy step
 * declares its need (mb_need); membudget() answers run, close the plugin first, or refuse. Pure and header-only (no
 * heap, no HAL): the session guard and the check-in over the link (app_session.c), the install check (plugin_host
 * tph_pkg.c), the TLS RX buffer choice (hal_esp tls_rx.h), the BLE open (hal_esp ble_heap.h) and the render guard
 * (app_render.c) and the floor before an HTTPS request (hal_esp tls_rx.h) call it; test/test_membudget.c pins every row.
 *
 * | Step | Block (one piece) | In all | Measured against | Short, plugin open | Short otherwise |
 * |---|---|---|---|---|---|
 * | check-in | caps net_block_bytes | caps net_free_bytes | largest free block, free heap | close the plugin | run (the session's own allocations fail clean: `E session nomem`) |
 * | check-in over the link | MB_LINK_BLOCK_BYTES | - | largest free block | close the plugin | run (remote's allocation fails clean: `LINK_E_NOMEM`) |
 * | install | - | the plugin's phase need | the board's budget (hal_board_t heap_render, heap_task, layer_budget) | refuse | refuse |
 * | render | the render's biggest piece | the least it runs with (scene state, arena, vector work, two thin bands) | largest free block, the bytes of the pieces the heap places now (app_mem_place) | refuse (the caller restarts for a fresh heap) | refuse |
 * | TLS RX | MB_TLS_RX_BYTES | - | largest free block | refuse (per-record buffers instead) | refuse |
 *   (only asked for a response with a body callback or a stream; one into the caller's buffer takes per-record
 *   buffers by choice, hal_esp src/tls_rx.h)
 * | TLS session | MB_TLS_BLOCK_BYTES | MB_TLS_FREE_BYTES | largest free block, free heap | refuse (`E tls nomem`, HAL_E_NOSPACE) | refuse |
 *   (before every HTTPS request, hal_esp esp/net.c: a handshake that runs out of memory half way is ESP-IDF 5.5.1's
 *   path with a freed buffer, issues #2 and #128; under the floor it is not started)
 * | BLE open | - | MB_BLE_OPEN_BYTES | free heap | refuse (`HAL_E_NOSPACE`) | refuse |
 *
 * No row, on purpose (issue #88): the firmware download (app_fwup.c closes the plugin and restarts for a fresh heap
 * before every download; its TLS buffer is the TLS RX row) and the wall state (checked in place in the session's
 * document and kept in a static buffer, app_wall.c: nothing is allocated). A refusal is the caller's clean error,
 * never a crash.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_MEMBUDGET_H
#define TINY_MEMBUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("tiny_caps.h") /* boards/<board>/caps.json via the generated tiny_caps.h; the host build has none */
#include "tiny_caps.h"
#endif
#endif

/* largest free block a check-in needs before WiFi starts: remote's buffers (27.3 KB, one block), then the largest
 * piece TLS takes next to WiFi's: a record buffer of 4.6 KB at most with our hosts (per-record buffers for the
 * check-in's answer, hal_esp src/tls_rx.h, issue #120; it was the 17 KB static buffer) (firmware.md "Sessions") */
#ifdef TINY_CAP_NET_BLOCK_BYTES
#define MB_NET_BLOCK TINY_CAP_NET_BLOCK_BYTES
#else
#define MB_NET_BLOCK (36u * 1024u)
#endif
/* free heap a whole check-in session needs (OG log 2026-10-04: remote + WiFi + TLS handshake took ~65 KB, 72 KB free
 * fell to 7 KB): peak ~65 KB + a 4.6 KB record buffer + headroom (it was + the 17 KB static buffer: 96 KiB). QEMU,
 * tools/idf/tlsprobe 2026-10-05: a check-in sized request to our host peaks at 19.5 KB (30.7 KB with the static
 * buffer) and holds 10.4 KB after the handshake (27.8 KB). Not yet measured on a device (issue #120).
 * Boards with PSRAM report MBs of free heap and never reach it. */
#ifdef TINY_CAP_NET_FREE_BYTES
#define MB_NET_FREE TINY_CAP_NET_FREE_BYTES
#else
#define MB_NET_FREE (84u * 1024u)
#endif
/* The static TLS RX buffer's allocation (SSL_BUF_HEAD_OFFSET_SIZE + the RX record length, 17058 B), with some room:
 * mbedTLS's, the same on every board. */
#define MB_TLS_RX_BYTES 17408u
/* The floor before an HTTPS request (issues #2, #128), mbedTLS's and ESP-IDF's, the same on every board (boards with
 * PSRAM never reach it). In one block: the handshake's TX buffer, allocated at ClientHello and again for every flight
 * (esp_ssl_cli.c manage_resource): SSL_BUF_HEAD_OFFSET_SIZE 8 + MBEDTLS_SSL_OUT_BUFFER_LEN (CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN
 * 4096 + the 333 B record overhead: header 13, IV 16, MAC 48, padding 256) + the port's 333 B once more
 * (tx_buffer_len) = 4770 B; the Certificate record of our hosts (two certificates, ~2.7 KB + 341) is smaller. In
 * all: the largest heap use measured on the OG for a request with a full handshake and per-record buffers (device
 * log 2026-10-05, www.trmnlplugins.xyz, 1.5 KB answer into the caller's buffer, `rx=dyn`: peak 30060 B; QEMU
 * tools/idf/tlsprobe: 19.5 KB) plus the largest response record buffer of our hosts (4253 + 341 = 4594 B). */
#define MB_TLS_BLOCK_BYTES 4770u
#define MB_TLS_FREE_BYTES (30060u + 4594u)
/* A check-in over the link (USB or BLE, no WiFi or TLS): remote's buffers (~28 KiB) in one piece. The same on every
 * board (the buffers are remote's, not the board's). */
#define MB_LINK_BLOCK_BYTES 32768u
/* Free heap the BLE stack needs to come up: controller + host + pools + ring. NimBLE's, the same on every board. */
#define MB_BLE_OPEN_BYTES (72u * 1024u)

typedef enum { MB_CHECKIN = 0, MB_LINK_CHECKIN, MB_INSTALL, MB_RENDER, MB_TLS_RX, MB_BLE_OPEN, MB_TLS_SESSION, MB_STEPS } mb_step_t;

typedef enum { MB_RUN = 0, MB_CLOSE_PLUGIN = 1, MB_REFUSE = 2 } mb_verdict_t;

/* The board's numbers (the capability table, issue #24). */
typedef struct {
    uint32_t net_block, net_free;
} mb_board_t;

/* What a step wants: its biggest single allocation and all of it. 0: no demand of that kind. */
typedef struct {
    uint64_t block, total;
} mb_need_t;

/* What there is: the largest free block, and the free heap, (install) the board's budget for the phase or (render)
 * the bytes of its pieces the heap places now: free heap in pieces too small does not count (issue #118).
 * SIZE_MAX: not tracked (the host), never short. */
typedef struct {
    bool plugin_open;
    size_t largest, avail;
} mb_have_t;

/* This build's board. */
static inline mb_board_t mb_board(void) { return (mb_board_t){MB_NET_BLOCK, MB_NET_FREE}; }

/* The need a step declares. b: the board (NULL: this build's). block, total: the plugin's part (install: the phase's need as `total`; render: its
 * biggest piece as `block`, all its pieces as `total`), ignored by the steps whose need is the board's. */
static inline mb_need_t mb_need(const mb_board_t *b, mb_step_t step, uint64_t block, uint64_t total)
{
    switch (step) {
    case MB_CHECKIN:
        return b ? (mb_need_t){b->net_block, b->net_free} : (mb_need_t){MB_NET_BLOCK, MB_NET_FREE};
    case MB_LINK_CHECKIN:
        return (mb_need_t){MB_LINK_BLOCK_BYTES, 0};
    case MB_TLS_RX:
        return (mb_need_t){MB_TLS_RX_BYTES, 0};
    case MB_TLS_SESSION:
        return (mb_need_t){MB_TLS_BLOCK_BYTES, MB_TLS_FREE_BYTES};
    case MB_BLE_OPEN:
        return (mb_need_t){0, MB_BLE_OPEN_BYTES};
    case MB_INSTALL:
        return (mb_need_t){0, total};
    case MB_RENDER:
    default:
        return (mb_need_t){block, total};
    }
}

/* The decision. An unknown step refuses. */
static inline mb_verdict_t membudget(mb_step_t step, mb_need_t need, const mb_have_t *have)
{
    if ((unsigned)step >= MB_STEPS)
        return MB_REFUSE;
    bool is_short = (need.block && have->largest < need.block) || (need.total && have->avail < need.total);
    if (!is_short)
        return MB_RUN;
    bool closable = step == MB_CHECKIN || step == MB_LINK_CHECKIN; /* the step runs without the plugin */
    if (!closable)
        return MB_REFUSE;
    return have->plugin_open ? MB_CLOSE_PLUGIN : MB_RUN; /* nothing to close: it tries, its allocations fail clean */
}

#endif
