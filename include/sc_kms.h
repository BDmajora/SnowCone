/*
 * snowcone/include/sc_kms.h — DRM/KMS framebuffer management.
 *
 * Owns the DRM device fd, mode selection, dumb-buffer creation, and
 * CRTC configuration. Exposes the pixel buffer that the rasterizer
 * draws into, plus dirty-rect flushing.
 */

#ifndef SC_KMS_H
#define SC_KMS_H

#include <stdint.h>
#include <drm/drm_mode.h>

typedef struct {
    int      fd;
    uint32_t conn_id;
    uint32_t crtc_id;
    uint32_t enc_id;
    struct drm_mode_modeinfo mode;
    uint32_t fb_id;
    uint32_t buf_handle;
    uint32_t pitch;         /* bytes per scanline */
    uint64_t size;          /* total mmap size    */
    uint32_t *pixels;       /* mmap'd ARGB8888 framebuffer */
} kms_t;

/* Dirty rectangle for partial display updates. */
typedef struct { int x, y, w, h; } rect_t;

/* Open the first DRM device with dumb-buffer support. */
int  kms_open(kms_t *k);

/* Pick the preferred display mode on the first connected output. */
int  kms_pick_mode(kms_t *k);

/* Allocate a dumb buffer and framebuffer object. */
int  kms_create_fb(kms_t *k);

/* Activate the CRTC with our framebuffer. */
int  kms_set_crtc(kms_t *k);

/* Mark a rectangle as needing a display update (advisory). */
void kms_dirty(kms_t *k, rect_t r);

/* Tear down the framebuffer, dumb buffer, and close the device. */
void kms_close(kms_t *k);

#endif /* SC_KMS_H */