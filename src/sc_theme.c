/*
 * snowcone/src/sc_raster.c — Software rasterizer.
 */

#include "sc_raster.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Filled rectangle                                                   */
/* ------------------------------------------------------------------ */

void fill_rect(kms_t *k, int x0, int y0, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    int x1 = x0 + w, y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)k->mode.hdisplay) x1 = k->mode.hdisplay;
    if (y1 > (int)k->mode.vdisplay) y1 = k->mode.vdisplay;
    int stride = k->pitch / 4;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = &k->pixels[y * stride];
        for (int x = x0; x < x1; x++) row[x] = c;
    }
}

/* ------------------------------------------------------------------ */
/* Filled polygon (2×2 supersampled, even-odd rule)                   */
/* ------------------------------------------------------------------ */

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

void fill_polygon(kms_t *k, const pt_t *v, int n, uint32_t color) {
    if (n < 3) return;

    float minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < n; i++) {
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
    }

    int iy0 = (int)(miny);       if (iy0 < 0) iy0 = 0;
    int iy1 = (int)(maxy) + 1;   if (iy1 > (int)k->mode.vdisplay) iy1 = k->mode.vdisplay;
    int ix0 = (int)(minx);       if (ix0 < 0) ix0 = 0;
    int ix1 = (int)(maxx) + 1;   if (ix1 > (int)k->mode.hdisplay) ix1 = k->mode.hdisplay;

    float xs[64];

    static const float subox[2] = { 0.25f, 0.75f };
    static const float suboy[2] = { 0.25f, 0.75f };

    for (int py = iy0; py < iy1; py++) {
        int cov_buf[4096];
        int row_w = ix1 - ix0;
        if (row_w <= 0) continue;
        if (row_w > 4096) row_w = 4096;
        for (int i = 0; i < row_w; i++) cov_buf[i] = 0;

        for (int sy = 0; sy < 2; sy++) {
            float scanY = (float)py + suboy[sy];
            int nx = 0;
            for (int i = 0; i < n; i++) {
                pt_t a = v[i], b = v[(i + 1) % n];
                if (a.y == b.y) continue;
                if ((a.y > scanY) == (b.y > scanY)) continue;
                float t = (scanY - a.y) / (b.y - a.y);
                if (nx < 64) xs[nx++] = a.x + t * (b.x - a.x);
            }
            if (nx < 2) continue;
            qsort(xs, nx, sizeof(float), cmp_float);

            for (int sx = 0; sx < 2; sx++) {
                for (int px = ix0; px < ix1; px++) {
                    float sampX = (float)px + subox[sx];
                    /* Even-odd rule: count how many crossings lie to the
                     * left of sampX. Odd count = inside. */
                    int crossings = 0;
                    for (int j = 0; j < nx; j++) {
                        if (xs[j] < sampX) crossings++;
                        else break; /* xs is sorted ascending */
                    }
                    if (crossings & 1) cov_buf[px - ix0]++;
                }
            }
        }

        for (int px = 0; px < row_w; px++) {
            if (cov_buf[px]) {
                int cov = cov_buf[px] * 255 / 4;
                blend_px(k, ix0 + px, py, color, cov);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Anti-aliased line (Xiaolin Wu)                                     */
/* ------------------------------------------------------------------ */

void draw_line_aa(kms_t *k, float x0, float y0, float x1, float y1, uint32_t c) {
    int steep = (y1 - y0 < 0 ? -(y1 - y0) : (y1 - y0)) >
                (x1 - x0 < 0 ? -(x1 - x0) : (x1 - x0));
    if (steep) {
        float t;
        t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    float dx = x1 - x0;
    float dy = y1 - y0;
    float grad = dx == 0 ? 1.0f : dy / dx;
    float y = y0 + grad * 0.5f;

    for (int x = (int)x0; x <= (int)x1; x++) {
        int iy = (int)y;
        float frac = y - iy;
        int cov_lo = (int)((1.0f - frac) * 255);
        int cov_hi = (int)(frac * 255);
        if (steep) {
            blend_px(k, iy,     x, c, cov_lo);
            blend_px(k, iy + 1, x, c, cov_hi);
        } else {
            blend_px(k, x, iy,     c, cov_lo);
            blend_px(k, x, iy + 1, c, cov_hi);
        }
        y += grad;
    }
}

/* ------------------------------------------------------------------ */
/* Thick stroked line                                                 */
/* ------------------------------------------------------------------ */

void draw_thick_line(kms_t *k, float x0, float y0, float x1, float y1,
                     float thickness, uint32_t c) {
    if (thickness < 1.0f) {
        draw_line_aa(k, x0, y0, x1, y1, c);
        return;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float len = (dx * dx + dy * dy);
    if (len <= 0) return;

    /* sqrt without libm — Newton's method */
    float s = len, prev;
    do { prev = s; s = (s + len / s) * 0.5f; } while (prev - s > 0.001f || s - prev > 0.001f);

    float nx = -dy / s, ny = dx / s;
    int steps = (int)thickness;
    for (int i = -steps / 2; i <= steps / 2; i++) {
        float ox = nx * (float)i * 0.5f;
        float oy = ny * (float)i * 0.5f;
        draw_line_aa(k, x0 + ox, y0 + oy, x1 + ox, y1 + oy, c);
    }
}

/* ------------------------------------------------------------------ */
/* Fill + outline                                                     */
/* ------------------------------------------------------------------ */

void fill_and_outline(kms_t *k, const pt_t *v, int n,
                      uint32_t fill, uint32_t outline, float thick) {
    fill_polygon(k, v, n, fill);
    for (int i = 0; i < n; i++) {
        const pt_t *a = &v[i];
        const pt_t *b = &v[(i + 1) % n];
        draw_thick_line(k, a->x, a->y, b->x, b->y, thick, outline);
    }
}