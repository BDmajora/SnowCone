/*
 * snowcone/src/sc_theme.c — Visual theme and scene content.
 *
 * ============================================================
 * THIS IS THE FILE TO EDIT when you want to change the splash:
 *   - Colors            → COL_* defines below
 *   - Logo / icon       → draw_icon()
 *   - Wordmark text     → draw_wordmark()
 *   - Copyright text    → draw_copyright_text()
 *   - Marquee bar       → draw_marquee_track(), sc_draw_marquee_frame()
 *   - Layout positions  → MARQUEE_*_DESIGN, and y-offsets in each fn
 * ============================================================
 *
 * Everything here works in a 1024×768 design space. The xform_t
 * (from sc_scene.h) handles scaling to the actual display.
 */

#include "sc_scene.h"
#include "sc_font.h"
#include "sc_raster.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Color palette (ARGB8888)                                           */
/* ------------------------------------------------------------------ */

#define COL_BLACK     0xff000000u
#define COL_WHITE     0xffffffffu
#define COL_BABYBLUE  0xff8ec5ffu
#define COL_DIM_BLUE  0xff5a8fcfu
#define COL_COPY      0xffbbbbbbu   /* copyright text */
#define COL_OUTLINE   0xff000000u   /* icon outlines  */

/* ------------------------------------------------------------------ */
/* Design-space transform                                             */
/* ------------------------------------------------------------------ */

xform_t sc_make_xform(const kms_t *k) {
    float sx = (float)k->mode.hdisplay / 1024.0f;
    float sy = (float)k->mode.vdisplay / 768.0f;
    float s  = sx < sy ? sx : sy;
    xform_t x;
    x.scale = s;
    x.ox = ((float)k->mode.hdisplay - 1024.0f * s) * 0.5f;
    x.oy = ((float)k->mode.vdisplay - 768.0f  * s) * 0.5f;
    return x;
}

/* ------------------------------------------------------------------ */
/* Logo / icon                                                        */
/*                                                                    */
/* Geometry is defined in design space and then transformed through a */
/* per-icon rotate+squish before being mapped to pixels by dp(). To   */
/* tweak shape: adjust ICON_SQUISH (1.0 = original width, <1 skinnier)*/
/* and ICON_TILT_DEG (negative = counter-clockwise lean).             */
/* ------------------------------------------------------------------ */

#define ICON_PIVOT_X     510.0f
#define ICON_PIVOT_Y     340.0f
#define ICON_SQUISH      0.55f          /* horizontal scale around pivot */
#define ICON_TILT_DEG   -8.0f           /* rotation around pivot (deg)   */

/* Small sin/cos approximation good enough for a fixed tilt — Taylor
 * series around 0 in radians. The angle here is small (~8°) so a few
 * terms give more than enough precision and we avoid pulling in libm. */
static float icon_sin(float r) {
    float r2 = r * r;
    return r * (1.0f - r2 * (1.0f / 6.0f - r2 * (1.0f / 120.0f)));
}
static float icon_cos(float r) {
    float r2 = r * r;
    return 1.0f - r2 * (0.5f - r2 * (1.0f / 24.0f - r2 * (1.0f / 720.0f)));
}

/* Apply per-icon squish + rotation in design space, then map to pixels. */
static pt_t ip(const xform_t *x, float dx, float dy) {
    static int   inited = 0;
    static float ca, sa;
    if (!inited) {
        float rad = ICON_TILT_DEG * 3.14159265f / 180.0f;
        ca = icon_cos(rad);
        sa = icon_sin(rad);
        inited = 1;
    }
    float lx = (dx - ICON_PIVOT_X) * ICON_SQUISH;
    float ly = (dy - ICON_PIVOT_Y);
    float rx = ICON_PIVOT_X + lx * ca - ly * sa;
    float ry = ICON_PIVOT_Y + lx * sa + ly * ca;
    return dp(x, rx, ry);
}

static void draw_icon(kms_t *k, const xform_t *x) {
    /* Skinny snowcone, narrowed by ICON_SQUISH and tilted by ICON_TILT_DEG.
     * Three layers drawn back to front with comic-book outlines. */
    float ot = 3.5f * x->scale;
    if (ot < 2.0f) ot = 2.0f;

    /* 1. Cone (inverted triangle), lengthened apex for sleek silhouette. */
    pt_t cone[3] = {
        ip(x, 450, 320),
        ip(x, 570, 320),
        ip(x, 510, 490),
    };
    fill_polygon(k, cone, 3, COL_BABYBLUE);

    /* Darker right wedge for volume. */
    pt_t cone_shadow[3] = {
        ip(x, 510, 320),
        ip(x, 570, 320),
        ip(x, 510, 490),
    };
    fill_polygon(k, cone_shadow, 3, COL_DIM_BLUE);

    /* Outline the cone silhouette. */
    for (int i = 0; i < 3; i++) {
        pt_t a = cone[i], b = cone[(i + 1) % 3];
        draw_thick_line(k, a.x, a.y, b.x, b.y, ot, COL_OUTLINE);
    }

    /* 2. Snow scoop with bumpy top. Same 18-vertex profile as before —
     * ICON_SQUISH narrows it to match the slimmer cone. */
    pt_t snow[18];
    int n = 0;
    snow[n++] = ip(x, 612, 320);
    snow[n++] = ip(x, 600, 332);
    snow[n++] = ip(x, 560, 336);
    snow[n++] = ip(x, 512, 338);
    snow[n++] = ip(x, 464, 336);
    snow[n++] = ip(x, 424, 332);
    snow[n++] = ip(x, 412, 320);
    snow[n++] = ip(x, 400, 296);
    snow[n++] = ip(x, 402, 264);
    snow[n++] = ip(x, 422, 232);
    snow[n++] = ip(x, 458, 208);
    snow[n++] = ip(x, 488, 196);
    snow[n++] = ip(x, 510, 204);
    snow[n++] = ip(x, 540, 192);
    snow[n++] = ip(x, 576, 200);
    snow[n++] = ip(x, 604, 228);
    snow[n++] = ip(x, 616, 268);
    snow[n++] = ip(x, 618, 304);
    fill_and_outline(k, snow, n, COL_WHITE, COL_OUTLINE, ot);

    /* 3. Ice shard accent. */
    pt_t shard[5] = {
        ip(x, 478, 244),
        ip(x, 506, 252),
        ip(x, 514, 282),
        ip(x, 494, 304),
        ip(x, 470, 280),
    };
    fill_and_outline(k, shard, 5, COL_BABYBLUE, COL_OUTLINE, ot * 0.75f);

    /* Tiny white highlight on the shard. */
    pt_t glint[3] = {
        ip(x, 484, 252),
        ip(x, 496, 256),
        ip(x, 486, 270),
    };
    fill_polygon(k, glint, 3, COL_WHITE);
}

/* ------------------------------------------------------------------ */
/* Wordmark                                                           */
/* ------------------------------------------------------------------ */

static void draw_wordmark(kms_t *k, const xform_t *x) {
    const char *part_a = "Yeti";
    const char *part_b = "OS";

    float text_scale = 4.0f * x->scale;
    float thick = 2.5f * x->scale;
    if (thick < 1.5f) thick = 1.5f;

    float wa = sc_text_width(text_scale, part_a);
    float wb = sc_text_width(text_scale, part_b);
    float total = wa + wb;

    float pen_y = x->oy + 470.0f * x->scale;
    float pen_x = ((float)k->mode.hdisplay - total) * 0.5f;

    pen_x += sc_draw_text(k, pen_x, pen_y, text_scale, thick, COL_WHITE, part_a);
    sc_draw_text(k, pen_x, pen_y, text_scale, thick, COL_BABYBLUE, part_b);
}

/* ------------------------------------------------------------------ */
/* Copyright                                                          */
/* ------------------------------------------------------------------ */

static void draw_copyright_text(kms_t *k, const xform_t *x) {
    float scale = 1.8f * x->scale;
    if (scale < 1.2f) scale = 1.2f;
    float thick = 1.6f * x->scale;
    if (thick < 1.5f) thick = 1.5f;

    /* Line spacing must clear glyph caps (14) + descenders (~6) = ~20 glyph
     * units, times the text scale. Use ~24 to leave a little breathing room. */
    float line_h = 24.0f * scale;

    float bx = x->ox + 40.0f * x->scale;
    /* Position so the second line's descenders still sit above the screen
     * edge: place the first baseline well clear of the bottom. */
    float by = x->oy + (768.0f - 80.0f) * x->scale - line_h;

    sc_draw_text(k, bx, by, scale, thick, COL_COPY,
                 "Copyright (C) 2026");
    sc_draw_text(k, bx, by + line_h, scale, thick, COL_COPY,
                 "YetiOS Project - GNU GPL v3.0");
}

/* ------------------------------------------------------------------ */
/* Marquee bar                                                        */
/* ------------------------------------------------------------------ */

#define MARQUEE_X_DESIGN     412.0f
#define MARQUEE_Y_DESIGN     560.0f
#define MARQUEE_W_DESIGN     200.0f
#define MARQUEE_H_DESIGN     14.0f

rect_t sc_marquee_rect(const xform_t *x) {
    rect_t r;
    r.x = (int)(x->ox + MARQUEE_X_DESIGN * x->scale);
    r.y = (int)(x->oy + MARQUEE_Y_DESIGN * x->scale);
    r.w = (int)(MARQUEE_W_DESIGN * x->scale);
    r.h = (int)(MARQUEE_H_DESIGN * x->scale);
    return r;
}

static void draw_marquee_track(kms_t *k, const xform_t *x) {
    rect_t r = sc_marquee_rect(x);
    /* Border */
    fill_rect(k, r.x - 1, r.y - 1, r.w + 2, 1, COL_DIM_BLUE);
    fill_rect(k, r.x - 1, r.y + r.h, r.w + 2, 1, COL_DIM_BLUE);
    fill_rect(k, r.x - 1, r.y, 1, r.h, COL_DIM_BLUE);
    fill_rect(k, r.x + r.w, r.y, 1, r.h, COL_DIM_BLUE);
    /* Empty track */
    fill_rect(k, r.x, r.y, r.w, r.h, COL_BLACK);
}

void sc_draw_marquee_frame(kms_t *k, const xform_t *x, float pos) {
    rect_t r = sc_marquee_rect(x);
    fill_rect(k, r.x, r.y, r.w, r.h, COL_BLACK);

    int slug_w = (int)(40.0f * x->scale);
    if (slug_w < 12) slug_w = 12;
    int slug_x = r.x + (int)((float)(r.w - slug_w) * pos);

    int bands = 4;
    int band_w = slug_w / (bands * 2);
    if (band_w < 1) band_w = 1;
    for (int i = 0; i < bands; i++) {
        int cov = (i + 1) * 255 / bands;
        for (int j = 0; j < band_w; j++) {
            int xl = slug_x + i * band_w + j;
            int xr = slug_x + slug_w - 1 - i * band_w - j;
            for (int y = r.y; y < r.y + r.h; y++) {
                blend_px(k, xl, y, COL_BABYBLUE, cov);
                if (xr != xl) blend_px(k, xr, y, COL_BABYBLUE, cov);
            }
        }
    }
    int core_x = slug_x + bands * band_w;
    int core_w = slug_w - 2 * bands * band_w;
    if (core_w > 0) fill_rect(k, core_x, r.y, core_w, r.h, COL_BABYBLUE);
}

/* ------------------------------------------------------------------ */
/* Public: draw the full static scene                                 */
/* ------------------------------------------------------------------ */

void sc_draw_static(kms_t *k, const xform_t *xf) {
    memset(k->pixels, 0, (size_t)k->size);
    draw_icon(k, xf);
    draw_wordmark(k, xf);
    draw_copyright_text(k, xf);
    draw_marquee_track(k, xf);
}