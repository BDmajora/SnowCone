// snowcone/src/sc_font.c — Hershey-style stroked vector font.

#include "sc_font.h"
#include "sc_raster.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char   c;
    int8_t width;
    int8_t pts[64]; // x,y pairs, terminated by 99,99
} glyph_t;

#define PU  -1, -1   // pen up
#define END  99, 99

static const glyph_t FONT[] = {
    // Wordmark
    {'Y', 8, {0,0, 4,7, 8,0, PU, 4,7, 4,14, END}},
    {'e', 7, {0,11, 7,11, 7,9, 6,7, 1,7, 0,9, 0,12, 2,14, 5,14, 7,12, END}},
    {'t', 5, {2,2, 2,12, 4,14, PU, 0,7, 5,7, END}},
    {'i', 2, {1,0, 1,2, PU, 1,5, 1,14, END}},
    {'O', 9, {2,0, 7,0, 9,3, 9,11, 7,14, 2,14, 0,11, 0,3, 2,0, END}},
    {'S', 8, {8,2, 6,0, 2,0, 0,2, 0,5, 2,7, 6,7, 8,9, 8,12, 6,14, 2,14, 0,12, END}},

    // Copyright + extras
    {'C', 8, {8,2, 6,0, 2,0, 0,2, 0,12, 2,14, 6,14, 8,12, END}},
    {'o', 7, {3,7, 6,7, 7,9, 7,12, 5,14, 2,14, 0,12, 0,9, 2,7, 3,7, END}},
    {'p', 7, {0,7, 0,18, PU, 0,8, 2,7, 5,7, 7,9, 7,12, 5,14, 2,14, 0,12, END}},
    {'y', 7, {0,7, 3,14, PU, 7,7, 3,14, 0,18, END}},
    {'r', 5, {0,14, 0,7, PU, 0,9, 2,7, 5,7, END}},
    {'g', 7, {7,7, 7,18, 5,20, 1,20, PU, 7,8, 5,7, 2,7, 0,9, 0,12, 2,14, 5,14, 7,12, END}},
    {'h', 6, {0,0, 0,14, PU, 0,9, 2,7, 5,7, 6,9, 6,14, END}},
    {'P', 6, {0,14, 0,0, 5,0, 6,2, 6,5, 5,7, 0,7, END}},
    {'j', 3, {2,0, 2,2, PU, 2,5, 2,17, 0,19, END}},
    {'c', 7, {7,9, 5,7, 2,7, 0,9, 0,12, 2,14, 5,14, 7,12, END}},
    {'G', 9, {9,2, 7,0, 2,0, 0,2, 0,12, 2,14, 7,14, 9,12, 9,7, 5,7, END}},
    {'N', 8, {0,14, 0,0, 8,14, 8,0, END}},
    {'U', 8, {0,0, 0,12, 2,14, 6,14, 8,12, 8,0, END}},
    {'L', 6, {0,0, 0,14, 6,14, END}},
    {'V', 8, {0,0, 4,14, 8,0, END}},
    {'0', 7, {3,0, 0,3, 0,11, 3,14, 4,14, 7,11, 7,3, 4,0, 3,0, END}},
    {'1', 4, {1,3, 3,0, 3,14, PU, 1,14, 5,14, END}},
    {'2', 7, {0,3, 3,0, 4,0, 7,3, 7,5, 0,14, 7,14, END}},
    {'3', 7, {0,2, 2,0, 5,0, 7,2, 7,5, 5,7, 3,7, PU, 5,7, 7,9, 7,12, 5,14, 2,14, 0,12, END}},
    {'4', 7, {5,0, 0,9, 7,9, PU, 5,5, 5,14, END}},
    {'5', 7, {7,0, 0,0, 0,7, 5,7, 7,9, 7,12, 5,14, 2,14, 0,12, END}},
    {'6', 7, {6,0, 2,0, 0,4, 0,12, 2,14, 5,14, 7,12, 7,9, 5,7, 2,7, 0,9, END}},
    {'7', 7, {0,0, 7,0, 4,7, 3,14, END}},
    {'8', 7, {2,0, 0,2, 0,5, 2,7, 5,7, 7,9, 7,12, 5,14, 2,14, 0,12, 0,9, 2,7, PU, 5,7, 7,5, 7,2, 5,0, 2,0, END}},
    {'9', 7, {7,2, 5,0, 2,0, 0,2, 0,5, 2,7, 5,7, 7,4, 7,12, 5,14, 1,14, END}},
    {'.', 2, {1,13, 1,14, END}},
    {',', 2, {1,13, 0,16, END}},
    {' ', 4, {END}},
    {'-', 6, {0,7, 6,7, END}},
    {'(', 4, {3,0, 2,1, 1,3, 0,6, 0,8, 1,11, 2,13, 3,14, END}},
    {')', 4, {0,0, 1,1, 2,3, 3,6, 3,8, 2,11, 1,13, 0,14, END}},
    {'v', 7, {0,7, 3,14, 6,7, END}},
};

#define FONT_COUNT (sizeof(FONT) / sizeof(FONT[0]))

// Linear scan; font table is small so this is fine.
static const glyph_t *find_glyph(char c) {
    for (size_t i = 0; i < FONT_COUNT; i++) {
        if (FONT[i].c == c) return &FONT[i];
    }
    return NULL;
}

// Draws a string and returns the horizontal advance in pixels.
float sc_draw_text(kms_t *k, float x, float y, float scale, float thickness,
                   uint32_t color, const char *s) {
    float pen_x = x;
    for (; *s; s++) {
        char c = *s;
        if (c == ' ') { pen_x += 4 * scale; continue; }
        const glyph_t *g = find_glyph(c);
        if (!g) { pen_x += 4 * scale; continue; }

        int pen_up = 1;
        float px = 0, py = 0;
        for (int i = 0; i < (int)sizeof(g->pts); i += 2) {
            int8_t a = g->pts[i], b = g->pts[i + 1];
            if (a == 99 && b == 99) break;
            if (a == -1 && b == -1) { pen_up = 1; continue; }
            float nx = pen_x + a * scale;
            float ny = y + b * scale;
            if (!pen_up)
                draw_thick_line(k, px, py, nx, ny, thickness, color);
            px = nx; py = ny;
            pen_up = 0;
        }
        pen_x += (g->width + 2) * scale; // +2 for inter-glyph spacing
    }
    return pen_x - x;
}

// Returns the pixel width of a string without drawing it.
float sc_text_width(float scale, const char *s) {
    float w = 0;
    for (; *s; s++) {
        if (*s == ' ') { w += 4 * scale; continue; }
        const glyph_t *g = find_glyph(*s);
        if (!g) { w += 4 * scale; continue; }
        w += (g->width + 2) * scale;
    }
    return w;
}