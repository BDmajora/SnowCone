/*
 * snowcone/include/sc_scene.h — Scene layout and coordinate transform.
 *
 * The scene is defined in a 1024×768 design space. The xform_t maps
 * design units to pixel coordinates, preserving aspect ratio with
 * letterboxing when the display dimensions differ.
 */

#ifndef SC_SCENE_H
#define SC_SCENE_H

#include "sc_kms.h"
#include "sc_raster.h"

/* Design-space → pixel-space transform. */
typedef struct {
    float scale;    /* design unit → pixel multiplier              */
    float ox, oy;   /* pixel offset of design (0,0) — letterbox    */
} xform_t;

/* Compute the transform for the current display mode. */
xform_t sc_make_xform(const kms_t *k);

/* Convert a design-space point to a pixel-space pt_t. */
static inline pt_t dp(const xform_t *x, float dx, float dy) {
    pt_t p = { x->ox + dx * x->scale, x->oy + dy * x->scale };
    return p;
}

/* ------------------------------------------------------------------ */
/* Theme entry points — implemented in sc_theme.c.                    */
/* Edit that file to change colors, logo, text, or marquee style.     */
/* ------------------------------------------------------------------ */

/* Draw the static scene (logo + wordmark + copyright + marquee track). */
void sc_draw_static(kms_t *k, const xform_t *xf);

/* Draw one frame of the marquee animation. pos is 0.0–1.0. */
void sc_draw_marquee_frame(kms_t *k, const xform_t *xf, float pos);

/* Return the pixel rect covered by the marquee (for dirty-flushing). */
rect_t sc_marquee_rect(const xform_t *xf);

#endif /* SC_SCENE_H */