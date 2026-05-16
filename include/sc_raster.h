/*
 * snowcone/include/sc_raster.h — Software rasterizer.
 *
 * All primitives draw into the kms_t pixel buffer in screen-space
 * coordinates. The scene layer is responsible for design-space →
 * pixel-space conversion.
 *
 * AA strategy: filled polygons use 2×2 supersampling; lines use
 * Wu-style fractional coverage.
 */

#ifndef SC_RASTER_H
#define SC_RASTER_H

#include "sc_kms.h"
#include <stdint.h>

/* A 2D point in pixel space (used by polygon fill and scene code). */
typedef struct { float x, y; } pt_t;

/* Write a single pixel (bounds-checked, no blend). */
static inline void put_px(kms_t *k, int x, int y, uint32_t argb) {
    if ((unsigned)x >= (unsigned)k->mode.hdisplay) return;
    if ((unsigned)y >= (unsigned)k->mode.vdisplay) return;
    k->pixels[y * (k->pitch / 4) + x] = argb;
}

/* Alpha-blend src over the existing pixel with 0–255 coverage. */
static inline void blend_px(kms_t *k, int x, int y, uint32_t src, int cov) {
    if ((unsigned)x >= (unsigned)k->mode.hdisplay) return;
    if ((unsigned)y >= (unsigned)k->mode.vdisplay) return;
    if (cov <= 0) return;
    if (cov >= 255) { put_px(k, x, y, src); return; }

    uint32_t *p = &k->pixels[y * (k->pitch / 4) + x];
    uint32_t d = *p;
    int sr = (src >> 16) & 0xff, sg = (src >> 8) & 0xff, sb = src & 0xff;
    int dr = (d   >> 16) & 0xff, dg = (d   >> 8) & 0xff, db = d   & 0xff;
    int r = (sr * cov + dr * (255 - cov)) / 255;
    int g = (sg * cov + dg * (255 - cov)) / 255;
    int b = (sb * cov + db * (255 - cov)) / 255;
    *p = 0xff000000u | (r << 16) | (g << 8) | b;
}

/* Solid-color filled rectangle (clipped to framebuffer bounds). */
void fill_rect(kms_t *k, int x0, int y0, int w, int h, uint32_t c);

/* Filled polygon with 2×2 supersampled anti-aliasing (even-odd rule). */
void fill_polygon(kms_t *k, const pt_t *v, int n, uint32_t color);

/* Anti-aliased line (Xiaolin Wu algorithm). */
void draw_line_aa(kms_t *k, float x0, float y0, float x1, float y1, uint32_t c);

/* Thick stroked line — draws parallel AA lines. */
void draw_thick_line(kms_t *k, float x0, float y0, float x1, float y1,
                     float thickness, uint32_t c);

/* Fill a polygon then stroke its outline. */
void fill_and_outline(kms_t *k, const pt_t *v, int n,
                      uint32_t fill, uint32_t outline, float thick);

#endif /* SC_RASTER_H */