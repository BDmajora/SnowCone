/*
 * snowcone/include/sc_font.h — Hershey-style stroked vector font.
 *
 * Each glyph is a list of (x,y) pairs in a 0–10 wide, 0–14 tall grid.
 * (-1,-1) = pen up, (99,99) = end-of-glyph. Only the characters needed
 * by the splash are defined.
 */

#ifndef SC_FONT_H
#define SC_FONT_H

#include "sc_kms.h"
#include <stdint.h>

/* Draw a string and return the horizontal advance in pixels. */
int   sc_draw_text(kms_t *k, float x, float y, float scale,
                   float thickness, uint32_t color, const char *s);

/* Measure the width of a string without drawing it. */
float sc_text_width(float scale, const char *s);

#endif /* SC_FONT_H */