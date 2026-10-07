/* See btn_queue.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "btn_queue.h"

#include <string.h>

void bq_reset(bq_t *q) { memset(q, 0, sizeof *q); }

bool bq_push(bq_t *q, uint64_t down, uint64_t up, uint8_t input, int8_t swipe, bool brush)
{
    if (q->n == BQ_LEN) {
        q->dropped++;
        return false;
    }
    q->p[(q->head + q->n) % BQ_LEN] = (bq_press_t){down, up > down ? up : down + 1, swipe, input, brush};
    q->n++;
    return true;
}

bool bq_take(bq_t *q, bq_press_t *out)
{
    if (!q->n)
        return false;
    *out = q->p[q->head];
    q->head = (uint8_t)((q->head + 1) % BQ_LEN);
    q->n--;
    return true;
}
