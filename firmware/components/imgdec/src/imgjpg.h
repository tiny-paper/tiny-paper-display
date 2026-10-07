/* imgdec: baseline JPEG (private to the component). SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IMGJPG_H
#define IMGJPG_H

#include "imgdec.h"

/* The file from its start; `pre` holds bytes already read (file offset pre_off), the rest comes through io.read. */
int imgjpg_decode(const imgdec_io_t *io, imgdec_info_t *info, const uint8_t *pre, size_t pre_n, uint32_t pre_off);
int imgjpg_probe(const uint8_t *b, size_t n, imgdec_info_t *out);
size_t imgjpg_need(const imgdec_info_t *info);

#endif
