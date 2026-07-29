// snowcone/include/sc_font.h - SnowCone text rendering.

#ifndef SC_FONT_H
#define SC_FONT_H

#include "sc_kms.h"
#include <stdint.h>

// Draw a string; returns horizontal advance in pixels.
float sc_draw_text(kms_t *k, float x, float y, float scale,
                   float thickness, uint32_t color, const char *s);

// Measure string width without drawing.
float sc_text_width(float scale, const char *s);

#endif // SC_FONT_H
