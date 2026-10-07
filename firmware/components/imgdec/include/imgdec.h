/*
 * imgdec - streaming PNG, BMP, GIF and baseline JPEG decoding into gray rows (docs/spec/assets.md "Images").
 *
 * Pull model: the decoder reads the file through io.read (a flash file on the device, memory in tests) and hands
 * every row as gray levels 0..15 (one byte a pixel) to io.row. The whole image is never held: PNG needs the
 * inflate window (<= 32 KiB, less for small images or a small zlib window) and two scanlines, BMP one row, JPEG
 * about 5 KiB of tables and one row of MCUs (optionally scaled down 2, 4 or 8 times in the IDCT); a progressive JPEG
 * also its coefficients (the whole image, fewer a block when scaled: assets.md "JPEG", capped by prog_max).
 * Everything is allocated through io.alloc and freed before imgdec_decode returns.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef IMGDEC_H
#define IMGDEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Board build capability (caps.json features.webp): no decoder is linked when zero. */
#ifndef IMGDEC_WEBP
#define IMGDEC_WEBP 1
#endif
#define IMGDEC_WEBP_FEATURE "webp"

enum { IMG_PNG = 1, IMG_BMP = 2, IMG_JPEG = 3, IMG_HEIF = 4, IMG_WEBP = 5, IMG_GIF = 6 };

enum {
    IMGDEC_OK = 0,
    IMGDEC_E_FORMAT = -1,   /* not a PNG, BMP, GIF or JPEG, or a kind this decoder does not take (interlaced
                             * PNG, RLE) */
    IMGDEC_E_CORRUPT = -2,  /* truncated or bad data (inflate, filters, sizes) */
    IMGDEC_E_NOMEM = -3,
    IMGDEC_E_IO = -4,       /* read failed */
    IMGDEC_E_ABORT = -5,    /* a callback said stop */
    IMGDEC_E_SIZE = -6,     /* over IMGDEC_MAX_W or IMGDEC_MAX_H (JPEG: IMGDEC_JPEG_MAX) */
    IMGDEC_E_PROGRESSIVE = -7, /* arithmetic, lossless, hierarchical or 12-bit JPEG; progressive when prog_max is 0
                                * or the build has no progressive decoder (Kconfig IMGDEC_PROGRESSIVE) */
};

#define IMGDEC_MAX_W 4096
#define IMGDEC_MAX_H 4096
/* JPEG: camera originals (docs/spec/image-fetch.md), decoded scaled down 2, 4 or 8 times in the IDCT */
#define IMGDEC_JPEG_MAX 16384

typedef struct {
    uint8_t format;         /* IMG_PNG, IMG_BMP, IMG_JPEG */
    uint16_t w, h;          /* JPEG from imgdec_probe: 0 when the frame header was not in the bytes given */
    uint8_t bits;           /* bits per pixel as stored (PNG: bit depth x channels; BMP: bpp; JPEG: 8 x components) */
    uint8_t gray_bits;      /* 1, 2, 4 when every pixel is a gray level of that depth (black/white = 1), else 8 */
    uint8_t orient;         /* EXIF orientation 1..8 (1: as stored; PNG, BMP and JPEG without EXIF) */
    uint8_t scale;          /* JPEG: rows come scaled down by 2^scale (0..3) */
    uint16_t rw, rh;        /* size of the rows delivered: ceil(w / 2^scale), ceil(h / 2^scale) */
    uint8_t color;          /* 1: the format carries color (PNG RGB, RGBA or palette; BMP palette, 16, 24, 32 bit;
                             * YCbCr JPEG): row8 with want_rgb gives RGB rows (the pixels may still all be gray) */
    uint8_t progressive;    /* JPEG: 1 progressive (SOF2), decoded only with imgdec_io_t.prog_max */
    uint8_t whole;          /* the image held whole (GIF: an interlaced frame's indices; WebP: lossless or alpha
                             * pixels), counted by imgdec_need; set by the decode */
} imgdec_info_t;

/* EXIF of a JPEG (APP1): strings NUL-terminated and trimmed, "" / 0 when absent. */
typedef struct {
    uint8_t orient;         /* 1..8, 0 absent */
    char make[24], model[40];
    char date[20];          /* DateTimeOriginal "YYYY:MM:DD HH:MM:SS" */
    char desc[64];          /* ImageDescription */
    uint16_t f_x10;         /* FNumber x 10, rounded */
    uint32_t exp_num, exp_den; /* ExposureTime as stored */
    uint16_t iso;           /* ISOSpeedRatings */
    uint16_t focal_x10;     /* FocalLength in mm x 10, rounded */
    bool gps;               /* lat/lon present */
    int32_t lat_e5, lon_e5; /* degrees x 1e5, south and west negative */
} imgdec_exif_t;

/* The header from the first bytes of a file (>= 29 for PNG, >= 54 for BMP, >= 10 for GIF, JPEG: from FF D8 FF on, w = h = 0 until
 * its frame header is among the bytes): enough to refuse a wrong size before a download ends. IMGDEC_OK,
 * IMGDEC_E_FORMAT, IMGDEC_E_PROGRESSIVE, IMGDEC_E_SIZE, or IMGDEC_E_CORRUPT when more bytes are needed. A progressive
 * JPEG is IMGDEC_OK with info.progressive set when the build decodes them (the caller refuses it without prog_max).
 * HEIF (HEIC, AVIF: an ISO BMFF `ftyp` box), and WebP when IMGDEC_WEBP=0, are IMGDEC_E_FORMAT with
 * out->format IMG_HEIF or IMG_WEBP; a valid WebP header still sets its size for the photo relay. */
int imgdec_probe(const uint8_t *b, size_t n, imgdec_info_t *out);

/* A JPEG's frame header from its bytes as they stream in (docs/spec/image-fetch.md "Probe"): every segment before it
 * (APPn, EXIF, ICC) skipped by its length, nothing kept. Zeroed to start; fed the file from its first byte, in order. */
typedef struct {
    uint16_t len;
    uint8_t st, m, k, sof[6];
} imgdec_scan_t;
/* 0: more bytes wanted; 1: out has the frame header (format IMG_JPEG, w, h, bits, color, progressive; orient 1);
 * < 0: IMGDEC_E_PROGRESSIVE (progressive without the decoder, lossless, arithmetic, 12-bit), IMGDEC_E_FORMAT (not 1 or
 * 3 components: CMYK; a scan before the frame), IMGDEC_E_SIZE, IMGDEC_E_CORRUPT. */
int imgdec_scan(imgdec_scan_t *s, const uint8_t *b, size_t n, imgdec_info_t *out);

typedef struct {
    /* Up to cap bytes of the file from where the last read ended: the count, 0 at the end, < 0 an error. */
    long (*read)(void *ctx, uint8_t *buf, size_t cap);
    /* After the header: non-zero stops the decode with IMGDEC_E_ABORT (e.g. a size that does not fit). */
    int (*info)(void *ctx, const imgdec_info_t *info);
    /* Row y of the image (0 = top; BMP bottom-up files come last row first), w gray levels 0..15. Non-zero stops. */
    int (*row)(void *ctx, uint16_t y, const uint8_t *gray, uint16_t w);
    void *(*alloc)(void *ctx, size_t n);
    void (*free)(void *ctx, void *p);
    void *ctx;
    /* Largest inflate window to allocate (0 = 32 KiB): a stream that references further back is IMGDEC_E_NOMEM. */
    uint32_t window_max;
    /* Optional: the next read starts at byte `off` of the file (0 = ok). With it, bottom-up BMP rows come top first
     * (the rows are read last first), so a raster is written front to back. */
    int (*seek)(void *ctx, uint32_t off);
    /* Optional 8-bit rows (photos): when set, rows go here instead of `row`: w samples of ch bytes (1 gray, 3 R, G,
     * B). JPEG: RGB only with want_rgb and 3 components (a gray JPEG stays ch 1), else its Y component (chroma is not
     * decoded). PNG, BMP, GIF: with want_rgb and info.color RGB (alpha over white), else gray levels x 17, ch 1. */
    int (*row8)(void *ctx, uint16_t y, const uint8_t *px, uint16_t w, uint8_t ch);
    bool want_rgb;
    /* JPEG with row8: the smallest rows wanted; the largest scale s (0..3) with ceil(w / 2^s) >= min_w and
     * ceil(h / 2^s) >= min_h is taken (0, 0: full size). */
    uint16_t min_w, min_h;
    /* JPEG: most bytes for the decoder's own allocations (0: no limit): a larger scale is taken to fit (row8 only),
     * else IMGDEC_E_NOMEM. */
    uint32_t mem_max;
    /* Optional: filled from a JPEG's EXIF before info() (offsets beyond the segment's first 384 bytes need seek). */
    imgdec_exif_t *exif;
    /* Optional, JPEG: the text of its first COM segment before the frame header (at most com_cap - 1 bytes,
     * NUL-terminated), read before info(); the caller sets com[0] = 0 (image-fetch.md "Relay": `tiny-relay/1`). */
    char *com;
    uint16_t com_cap;
    /* Optional, JPEG with row8: called once the frame header and EXIF are read (w, h as stored, orient 1..8) and
     * before the scale is picked; it may change *min_w, *min_h (they start as min_w, min_h above). */
    void (*want)(void *ctx, uint16_t w, uint16_t h, uint8_t orient, uint16_t *min_w, uint16_t *min_h);
    /* Optional, JPEG with row8 (walls, assets.md "Photos"): for rows of rw x rh (a candidate scale) the columns wanted,
     * *x0 .. *x0 + *n - 1 (they start as 0, rw). Rows then hold only those n pixels (row8 gets w = n, px[0] = column
     * x0), MCUs outside them are not transformed, and the memory limit counts n. */
    void (*window)(void *ctx, uint16_t rw, uint16_t rh, uint16_t *x0, uint16_t *n);
    /* Progressive JPEG: most bytes for its coefficient store (imgdec_prog_store); 0: refused, IMGDEC_E_PROGRESSIVE.
     * With row8 a larger scale is taken to fit, else IMGDEC_E_NOMEM. */
    uint32_t prog_max;
    /* Progressive JPEG with seek: when its whole store does not fit prog_max (or mem_max), decoded in bands of rows,
     * every scan resumed where the band before left it (assets.md "JPEG" Progressive bands): the store of a band, the
     * scan index and a row within mem_max (0: prog_max); rows then come band by band (a damaged scan fails after the
     * bands before it). */
    bool prog_bands;
    /* Progressive JPEG with row8, when neither prog_max nor prog_bands with seek applies (a stream): only the first
     * scan, when it is the DC of every component (Ss = Se = 0, Ah = 0, in frame order), at 1/8 scale (the block
     * means), then the decode ends without reading the rest; its rows must be at least 5/8 of min_w x min_h (after
     * `want`), else IMGDEC_E_PROGRESSIVE (assets.md "JPEG" Progressive DC). */
    bool prog_dc;
} imgdec_io_t;

/* True when this build decodes progressive JPEGs (Kconfig IMGDEC_PROGRESSIVE; host and sim builds: always). */
bool imgdec_progressive(void);

/* Bytes of a progressive JPEG's coefficient store at scale s (0..3): per block kept, 128 at s 0, 40 at s 1 (the low
 * 4x4 and a nonzero bitmap), 16 at s 2 (2x2 and the bitmap), 2 at s 3 (DC); blocks of Y only (rgb false or a gray
 * file), else of every component, padded to whole MCUs. hs, vs: luma sampling 1..2 (chroma 1x1). */
uint64_t imgdec_prog_store(uint16_t w, uint16_t h, uint8_t hs, uint8_t vs, uint8_t nc, bool rgb, int s);

/* Decodes the whole file. Peak memory: imgdec_need(). */
int imgdec_decode(const imgdec_io_t *io, imgdec_info_t *info);

/* Bytes imgdec_decode allocates at most for an image with this header (window as the stream's zlib header would
 * allow: pass 32768 when unknown; JPEG: RGB rows of rw (or w), 4:2:0 MCUs). */
size_t imgdec_need(const imgdec_info_t *info, uint32_t window);

/* Gray (0..15) of an sRGB color: the host-abi.md "Colors" luminance. */
static inline uint8_t imgdec_gray_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)(((54u * r + 183u * g + 19u * b) >> 8) >> 4);
}

/* ------------------------------------------------------------------ rasters for a screen (assets.md "Fitting") */

/* Gray rows (as imgdec_io_t.row gives them; top first for writes front to back) into packed rows of a W x H screen at depth 1, 2 or 4: the
 * same size 1:1, or with `fit` any other size downscaled (never up) and centered on white. Memory: the packed row,
 * and for a downscale a row of sums and counts (8 bytes a pixel of the output width). */
typedef struct {
    uint16_t W, H;
    uint8_t depth;
    bool fit;
    /* Bytes of the raster (rows top to bottom, stride ceil(W * depth / 8)) at off. 0 = ok. */
    int (*write)(void *ctx, uint32_t off, const void *buf, size_t len);
    void *(*alloc)(void *ctx, size_t n);
    void (*free)(void *ctx, void *p);
    void *ctx;
    /* Optional wall view (assets.md "Fitting" Wall): W x H is the canvas, only its columns vx..vx+vw-1 and rows
     * vy..vy+vh-1 are written, as a vw x vh raster (the same values); vw 0: the whole screen. */
    uint16_t vx, vy, vw, vh;
    /* Walls: fit also scales up (s = min(W / w, H / h)), bilinear (assets.md "Fitting" Wall); else never up. */
    bool up;
    /* An image relay answer (assets.md "Fitting" Relay): the rows go 1:1 to (px, py) of the view (without one: of the
     * screen), every pixel by the fitted rule (a = 16 g, Bayer at the canvas position) at every depth; the rest white.
     * (px, py) at or past the view's size: white only. */
    bool placed;
    uint16_t px, py;
    /* state */
    uint16_t w, h, ow, oh, ox, oy;
    uint16_t *hr, *prev; /* bilinear: the resampled source row and the one before (x16 values, view columns) */
    uint16_t next;       /* bilinear: the next output row */
    uint16_t rw, rh;   /* the raster: W x H, or the view */
    uint16_t sx0, sn;  /* the output columns summed: sx0 .. sx0+sn-1 of 0..ow-1 */
    uint32_t stride;
    uint8_t *row;
    uint32_t *sum;
    uint16_t *cnt;
    int32_t cur;       /* output row being summed, -1 none */
    uint32_t rows_out;
} imgfit_t;

#define IMGFIT_E_SIZE (-10) /* another size and no fit */

/* After the header (imgdec_io_t.info). IMGDEC_OK, IMGFIT_E_SIZE, IMGDEC_E_NOMEM, IMGDEC_E_IO (a write failed). */
int imgfit_begin(imgfit_t *f, const imgdec_info_t *in);
/* One gray row (imgdec_io_t.row). */
int imgfit_row(imgfit_t *f, uint16_t y, const uint8_t *gray);
/* After the last row: every output row written. Frees what begin allocated (also call it after a failure). */
int imgfit_end(imgfit_t *f, bool ok);
/* Output size and position for an image w x h on the screen (s = min(1, W/w, H/h)). */
void imgfit_geometry(uint16_t w, uint16_t h, uint16_t W, uint16_t H, uint16_t *ow, uint16_t *oh, uint16_t *ox,
                     uint16_t *oy);

#endif
