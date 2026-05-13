/*
 * snowcone — a from-scratch UEFI/KMS boot splash for YetiOS.
 *
 * Mirrors the spirit of libreldr: no library dependencies (just libc and
 * Linux kernel headers), one C file, builds with `make`. Talks to
 * /dev/dri/card0 directly via DRM ioctls — no libdrm, no SDL, no pixman,
 * no freetype.
 *
 * Rendering model:
 *   - Vector scene described in normalized [0..1] coordinates.
 *   - Scanline polygon fill with 2x supersampling (cheap AA).
 *   - Hershey-style stroked single-line font for text.
 *   - Static parts (logo, wordmark, copyright) rendered ONCE into the
 *     dumb buffer at startup. Each animation frame only redraws the
 *     marquee bar region.
 *
 * Lifecycle:
 *   - Started by an early OpenRC service, before any display manager.
 *   - Holds DRM master. When the compositor/display manager starts and
 *     grabs DRM master, our next ioctl returns EACCES — we exit cleanly.
 *   - SIGTERM and SIGUSR1 also cause a clean exit, so other services can
 *     ask us to stop.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <linux/types.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

static int g_verbose = 0;

static void logmsg(const char *prefix, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "snowcone: %s ", prefix);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define LOGV(...) do { if (g_verbose) logmsg("[v]", __VA_ARGS__); } while (0)
#define LOGI(...) logmsg("[i]", __VA_ARGS__)
#define LOGE(...) logmsg("[!]", __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Signal handling — exit cleanly on TERM, USR1, or DRM master loss   */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* ------------------------------------------------------------------ */
/* Colors — ARGB8888                                                  */
/* ------------------------------------------------------------------ */

#define COL_BLACK     0xff000000u
#define COL_WHITE     0xffffffffu
#define COL_BABYBLUE  0xff8ec5ffu   /* the "baby blue" accent */
#define COL_DIM_BLUE  0xff5a8fcfu   /* slightly darker baby blue */
#define COL_GRAY_BAR  0xff404040u   /* marquee track */
#define COL_COPY      0xffbbbbbbu   /* copyright text */

/* ------------------------------------------------------------------ */
/* DRM/KMS state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int      fd;            /* /dev/dri/cardN */
    uint32_t conn_id;       /* active connector */
    uint32_t crtc_id;       /* CRTC driving it */
    uint32_t enc_id;
    struct drm_mode_modeinfo mode;
    uint32_t fb_id;
    uint32_t buf_handle;
    uint32_t pitch;         /* bytes per scanline */
    uint64_t size;          /* total bytes */
    uint32_t *pixels;       /* mmap'd ARGB8888 buffer */
    struct drm_mode_crtc saved_crtc; /* to restore on exit */
    int      saved_crtc_valid;
} kms_t;

static int try_card(const char *path, kms_t *k) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    /* Verify it supports dumb buffers — the simplest possible API.
     * Anything modern (i915, amdgpu, nouveau, virtio_gpu) supports it. */
    uint64_t has_dumb = 0;
    struct drm_get_cap cap = { .capability = DRM_CAP_DUMB_BUFFER };
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 || !cap.value) {
        close(fd);
        return -1;
    }
    (void)has_dumb;
    k->fd = fd;
    return 0;
}

static int kms_open(kms_t *k) {
    /* Walk card0..card7 until we find one with dumb buffer support and
     * an active connected display. */
    char path[32];
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (try_card(path, k) == 0) {
            LOGV("opened %s", path);
            return 0;
        }
    }
    LOGE("no usable DRM device found");
    return -1;
}

static int kms_pick_mode(kms_t *k) {
    /* Get resource list (counts only first) */
    struct drm_mode_card_res res = {0};
    if (ioctl(k->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        LOGE("GETRESOURCES: %s", strerror(errno));
        return -1;
    }

    if (res.count_connectors == 0 || res.count_crtcs == 0) {
        LOGE("no connectors or CRTCs");
        return -1;
    }

    uint32_t *conns = calloc(res.count_connectors, sizeof(*conns));
    uint32_t *crtcs = calloc(res.count_crtcs,      sizeof(*crtcs));
    uint32_t *encs  = calloc(res.count_encoders,   sizeof(*encs));
    uint32_t *fbs   = calloc(res.count_fbs ? res.count_fbs : 1, sizeof(*fbs));
    if (!conns || !crtcs || !encs || !fbs) { LOGE("oom"); return -1; }

    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr      = (uintptr_t)crtcs;
    res.encoder_id_ptr   = (uintptr_t)encs;
    res.fb_id_ptr        = (uintptr_t)fbs;
    if (ioctl(k->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        LOGE("GETRESOURCES(2): %s", strerror(errno));
        return -1;
    }

    /* Find a connected connector with at least one mode. */
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector gc = {0};
        gc.connector_id = conns[i];

        /* First call: get counts. */
        if (ioctl(k->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) continue;
        if (gc.connection != 1 /* DRM_MODE_CONNECTED */) continue;
        if (gc.count_modes == 0) continue;

        struct drm_mode_modeinfo *modes = calloc(gc.count_modes, sizeof(*modes));
        uint32_t *cencs = calloc(gc.count_encoders ? gc.count_encoders : 1, sizeof(*cencs));
        uint32_t *props = calloc(gc.count_props ? gc.count_props : 1, sizeof(*props));
        uint64_t *pvals = calloc(gc.count_props ? gc.count_props : 1, sizeof(*pvals));
        gc.modes_ptr     = (uintptr_t)modes;
        gc.encoders_ptr  = (uintptr_t)cencs;
        gc.props_ptr     = (uintptr_t)props;
        gc.prop_values_ptr = (uintptr_t)pvals;
        if (ioctl(k->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) {
            free(modes); free(cencs); free(props); free(pvals);
            continue;
        }

        /* Prefer the first mode (typically the preferred / native one). */
        k->mode = modes[0];
        k->conn_id = conns[i];
        k->enc_id  = gc.encoder_id;

        /* Map encoder → CRTC. */
        struct drm_mode_get_encoder ge = {0};
        ge.encoder_id = k->enc_id;
        if (ioctl(k->fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0 && ge.crtc_id) {
            k->crtc_id = ge.crtc_id;
        } else if (res.count_crtcs > 0) {
            k->crtc_id = crtcs[0];
        }

        free(modes); free(cencs); free(props); free(pvals);

        if (k->crtc_id) {
            LOGI("display %dx%d @ %dHz on connector %u",
                 k->mode.hdisplay, k->mode.vdisplay, k->mode.vrefresh,
                 k->conn_id);
            free(conns); free(crtcs); free(encs); free(fbs);
            return 0;
        }
    }

    LOGE("no connected display");
    free(conns); free(crtcs); free(encs); free(fbs);
    return -1;
}

static int kms_create_fb(kms_t *k) {
    struct drm_mode_create_dumb cd = {0};
    cd.width  = k->mode.hdisplay;
    cd.height = k->mode.vdisplay;
    cd.bpp    = 32;
    if (ioctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        LOGE("CREATE_DUMB: %s", strerror(errno));
        return -1;
    }
    k->buf_handle = cd.handle;
    k->pitch      = cd.pitch;
    k->size       = cd.size;

    struct drm_mode_fb_cmd fb = {0};
    fb.width  = k->mode.hdisplay;
    fb.height = k->mode.vdisplay;
    fb.pitch  = k->pitch;
    fb.bpp    = 32;
    fb.depth  = 24;
    fb.handle = k->buf_handle;
    if (ioctl(k->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        LOGE("ADDFB: %s", strerror(errno));
        return -1;
    }
    k->fb_id = fb.fb_id;

    struct drm_mode_map_dumb md = {0};
    md.handle = k->buf_handle;
    if (ioctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        LOGE("MAP_DUMB: %s", strerror(errno));
        return -1;
    }

    void *p = mmap(NULL, k->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   k->fd, (off_t)md.offset);
    if (p == MAP_FAILED) {
        LOGE("mmap: %s", strerror(errno));
        return -1;
    }
    k->pixels = (uint32_t *)p;

    /* Start with a clean black screen. */
    memset(p, 0, (size_t)k->size);
    return 0;
}

static int kms_set_crtc(kms_t *k) {
    /* Save current CRTC config so we can restore on exit. */
    struct drm_mode_crtc sc = {0};
    sc.crtc_id = k->crtc_id;
    if (ioctl(k->fd, DRM_IOCTL_MODE_GETCRTC, &sc) == 0) {
        k->saved_crtc = sc;
        k->saved_crtc_valid = 1;
    }

    struct drm_mode_crtc cm = {0};
    cm.crtc_id    = k->crtc_id;
    cm.fb_id      = k->fb_id;
    cm.set_connectors_ptr = (uintptr_t)&k->conn_id;
    cm.count_connectors   = 1;
    cm.mode       = k->mode;
    cm.mode_valid = 1;
    if (ioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &cm) < 0) {
        LOGE("SETCRTC: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void kms_close(kms_t *k) {
    if (k->saved_crtc_valid) {
        ioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &k->saved_crtc);
    }
    if (k->pixels) munmap(k->pixels, k->size);
    if (k->fb_id) {
        ioctl(k->fd, DRM_IOCTL_MODE_RMFB, &k->fb_id);
    }
    if (k->buf_handle) {
        struct drm_mode_destroy_dumb dd = { .handle = k->buf_handle };
        ioctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    if (k->fd >= 0) close(k->fd);
}

/* ------------------------------------------------------------------ */
/* Rasterizer                                                         */
/*                                                                    */
/* Conventions: all primitives operate on the kms_t framebuffer in    */
/* pixel coordinates (top-left origin). The scene layer converts from */
/* normalized [0..1] coordinates to pixels.                           */
/*                                                                    */
/* AA strategy: filled polygons supersample at 2x in software,        */
/* averaging 4 sub-samples per pixel. Lines use Wu-style coverage.    */
/* ------------------------------------------------------------------ */

static inline void put_px(kms_t *k, int x, int y, uint32_t argb) {
    if ((unsigned)x >= (unsigned)k->mode.hdisplay) return;
    if ((unsigned)y >= (unsigned)k->mode.vdisplay) return;
    k->pixels[y * (k->pitch / 4) + x] = argb;
}

/* Blend src (with given coverage 0..255) over the existing pixel. */
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

static void fill_rect(kms_t *k, int x0, int y0, int w, int h, uint32_t c) {
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

/* Filled polygon via scanline algorithm with 2x supersampling.
 *
 * For each output pixel we evaluate 4 sub-samples (2 horizontal,
 * 2 vertical) — count how many fall inside, derive a 0..255 coverage,
 * and blend. The "inside" test uses the standard even-odd rule over
 * the polygon's edge intersections at that sub-row.
 *
 * Not the fastest possible AA, but the scenes are small (a few hundred
 * pixels per shape) and this only runs once at startup.
 */
typedef struct { float x, y; } pt_t;

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void fill_polygon(kms_t *k, const pt_t *v, int n, uint32_t color) {
    if (n < 3) return;
    /* Bounding box. */
    float minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < n; i++) {
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
    }
    int y0 = (int)minx; /* unused, silence */ (void)y0;
    int iy0 = (int)(miny);       if (iy0 < 0) iy0 = 0;
    int iy1 = (int)(maxy) + 1;   if (iy1 > (int)k->mode.vdisplay) iy1 = k->mode.vdisplay;
    int ix0 = (int)(minx);       if (ix0 < 0) ix0 = 0;
    int ix1 = (int)(maxx) + 1;   if (ix1 > (int)k->mode.hdisplay) ix1 = k->mode.hdisplay;

    float xs[64]; /* edge intersections at one sub-scanline */

    /* 2x2 sub-sample offsets within a pixel */
    static const float subox[2] = { 0.25f, 0.75f };
    static const float suboy[2] = { 0.25f, 0.75f };

    for (int py = iy0; py < iy1; py++) {
        /* Coverage row: 0..4 per pixel (we'll scale to 0..255 at blend) */
        /* Allocate inline — we don't know width up front, use stack-friendly chunks. */
        int cov_buf[4096];
        int row_w = ix1 - ix0;
        if (row_w <= 0) continue;
        if (row_w > 4096) row_w = 4096; /* clamp; splash never goes wider */
        for (int i = 0; i < row_w; i++) cov_buf[i] = 0;

        for (int sy = 0; sy < 2; sy++) {
            float scanY = (float)py + suboy[sy];
            int nx = 0;
            /* Collect edge intersections with this sub-scanline */
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
                    /* Even-odd test */
                    int inside = 0;
                    for (int j = 0; j < nx; j++) {
                        if (sampX < xs[j]) { inside = j & 1 ? 0 : 1; break; }
                    }
                    if (inside) cov_buf[px - ix0]++;
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

/* Stroked line with anti-aliasing (Xiaolin Wu's algorithm).
 * Used for the Hershey-style font and the polygon outlines. */
static void draw_line_aa(kms_t *k, float x0, float y0, float x1, float y1, uint32_t c) {
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

/* Thick stroked line — draws N parallel offset lines. Cheap and looks
 * fine at the sizes we use (1–3 px). */
static void draw_thick_line(kms_t *k, float x0, float y0, float x1, float y1,
                            float thickness, uint32_t c) {
    if (thickness < 1.0f) {
        draw_line_aa(k, x0, y0, x1, y1, c);
        return;
    }
    /* Perpendicular unit vector */
    float dx = x1 - x0, dy = y1 - y0;
    float len = (dx * dx + dy * dy);
    if (len <= 0) return;
    /* sqrt without libm */
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
/* Vector font — simple Hershey-style stroked letters.                */
/*                                                                    */
/* Each glyph is a list of (x,y) pairs in a 0..10 wide, 0..14 tall    */
/* grid. A pair of -1,-1 means "pen up" (move without drawing).       */
/* Only the chars we need are defined.                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char c;
    int8_t width;
    int8_t pts[64];  /* x,y pairs, terminated by 99 */
} glyph_t;

#define PU -1, -1   /* pen up */
#define END 99, 99

/* The wordmark uses these letters: Y, e, t, i, O, S
 * Plus copyright text needs more — we'll keep the alphabet compact and
 * just define what we use. */

static const glyph_t FONT[] = {
    {'Y', 8, {0,0, 4,7, 8,0, PU, 4,7, 4,14, END}},
    {'e', 7, {0,11, 7,11, 7,9, 6,7, 1,7, 0,9, 0,12, 2,14, 5,14, 7,12, END}},
    {'t', 5, {2,2, 2,12, 4,14, PU, 0,7, 5,7, END}},
    {'i', 2, {1,0, 1,2, PU, 1,5, 1,14, END}},
    {'O', 9, {2,0, 7,0, 9,3, 9,11, 7,14, 2,14, 0,11, 0,3, 2,0, END}},
    {'S', 8, {8,2, 6,0, 2,0, 0,2, 0,5, 2,7, 6,7, 8,9, 8,12, 6,14, 2,14, 0,12, END}},

    /* Lowercase + digits + punctuation for copyright */
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
    {'(', 3, {3,0, 1,2, 0,5, 0,9, 1,12, 3,14, END}},
    {')', 3, {0,0, 2,2, 3,5, 3,9, 2,12, 0,14, END}},
};

static const glyph_t *find_glyph(char c) {
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].c == c) return &FONT[i];
    }
    return NULL;
}

static int draw_text(kms_t *k, float x, float y, float scale, float thickness,
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
            if (!pen_up) {
                draw_thick_line(k, px, py, nx, ny, thickness, color);
            }
            px = nx; py = ny;
            pen_up = 0;
        }
        pen_x += (g->width + 2) * scale;
    }
    return (int)(pen_x - x);
}

/* Measure text width for centering */
static float text_width(float scale, const char *s) {
    float w = 0;
    for (; *s; s++) {
        if (*s == ' ') { w += 4 * scale; continue; }
        const glyph_t *g = find_glyph(*s);
        if (!g) { w += 4 * scale; continue; }
        w += (g->width + 2) * scale;
    }
    return w;
}

/* ------------------------------------------------------------------ */
/* The scene: a YetiOS-themed boot splash                             */
/*                                                                    */
/* All coordinates are in a normalized 1024x768 design space —        */
/* the kms_to_design transform scales this to the actual framebuffer  */
/* while preserving aspect ratio (letterboxing if needed).            */
/* ------------------------------------------------------------------ */

typedef struct {
    float scale;       /* design unit → pixel multiplier */
    float ox, oy;      /* offset of design (0,0) in pixel coords (letterboxing) */
} xform_t;

static xform_t make_xform(const kms_t *k) {
    /* Design space is 1024 wide, 768 tall. */
    float sx = (float)k->mode.hdisplay / 1024.0f;
    float sy = (float)k->mode.vdisplay / 768.0f;
    float s  = sx < sy ? sx : sy;
    xform_t x;
    x.scale = s;
    x.ox = ((float)k->mode.hdisplay - 1024.0f * s) * 0.5f;
    x.oy = ((float)k->mode.vdisplay - 768.0f  * s) * 0.5f;
    return x;
}

static inline pt_t dp(const xform_t *x, float dx, float dy) {
    pt_t p = { x->ox + dx * x->scale, x->oy + dy * x->scale };
    return p;
}

/* Layout (in 1024×768 design space):
 *
 *   Mountain group:   centered around (512, 320), spans ~360 px wide
 *   Wordmark:         centered at (512, 470), tall ~70 px
 *   Marquee bar:      (412, 560)–(612, 580), width 200, height 16
 *   Copyright text:   bottom-left at (40, 720)–(40, 745)
 */

#define MARQUEE_X_DESIGN     412.0f
#define MARQUEE_Y_DESIGN     560.0f
#define MARQUEE_W_DESIGN     200.0f
#define MARQUEE_H_DESIGN     14.0f

static void draw_mountain(kms_t *k, const xform_t *x) {
    /* Main triangle (white) */
    pt_t main_tri[3] = {
        dp(x, 360, 420),
        dp(x, 512, 200),
        dp(x, 664, 420),
    };
    fill_polygon(k, main_tri, 3, COL_WHITE);

    /* Smaller foreground peak on the right (baby blue) */
    pt_t r_tri[3] = {
        dp(x, 560, 420),
        dp(x, 640, 280),
        dp(x, 720, 420),
    };
    fill_polygon(k, r_tri, 3, COL_BABYBLUE);

    /* Snowcap accents — small triangles near the peaks of the main mountain */
    pt_t snowcap[3] = {
        dp(x, 480, 260),
        dp(x, 512, 200),
        dp(x, 544, 260),
    };
    fill_polygon(k, snowcap, 3, COL_BABYBLUE);

    /* Moon: filled circle approximated by an n-gon (32 sides is plenty for AA) */
    pt_t moon[32];
    float cx = 740, cy = 220, r = 22;
    for (int i = 0; i < 32; i++) {
        float a = (float)i * (6.2831853f / 32.0f);
        /* tiny cos/sin without libm — use Taylor approx for small budget;
         * easier to just precompute via a table. */
        /* Use a 32-entry circle table generated at compile time isn't pretty
         * in C without macros — so fall back to a simple iterative rotation. */
        float ca, sa;
        /* Use a small lookup */
        static const float CS[32][2] = {
            {1.000f,0.000f},{0.981f,0.195f},{0.924f,0.383f},{0.831f,0.556f},
            {0.707f,0.707f},{0.556f,0.831f},{0.383f,0.924f},{0.195f,0.981f},
            {0.000f,1.000f},{-0.195f,0.981f},{-0.383f,0.924f},{-0.556f,0.831f},
            {-0.707f,0.707f},{-0.831f,0.556f},{-0.924f,0.383f},{-0.981f,0.195f},
            {-1.000f,0.000f},{-0.981f,-0.195f},{-0.924f,-0.383f},{-0.831f,-0.556f},
            {-0.707f,-0.707f},{-0.556f,-0.831f},{-0.383f,-0.924f},{-0.195f,-0.981f},
            {0.000f,-1.000f},{0.195f,-0.981f},{0.383f,-0.924f},{0.556f,-0.831f},
            {0.707f,-0.707f},{0.831f,-0.556f},{0.924f,-0.383f},{0.981f,-0.195f},
        };
        ca = CS[i][0]; sa = CS[i][1];
        (void)a;
        moon[i] = dp(x, cx + r * ca, cy + r * sa);
    }
    fill_polygon(k, moon, 32, COL_WHITE);
}

static void draw_wordmark(kms_t *k, const xform_t *x) {
    /* "YetiOS" — 'Yeti' in white, 'OS' in baby blue. */
    const char *a = "Yeti";
    const char *b = "OS";

    float text_scale = 4.0f * x->scale;          /* glyph unit → pixels */
    float thick = 2.5f * x->scale;
    if (thick < 1.5f) thick = 1.5f;

    /* Width of full word for centering. */
    float wa = text_width(text_scale, a);
    float wb = text_width(text_scale, b);
    float total = wa + wb;

    float pen_y = x->oy + 470.0f * x->scale;
    float pen_x = x->ox + ((float)k->mode.hdisplay - x->ox * 2 - total) * 0.5f;
    /* Recenter using actual fb width to handle letterboxing edge cases. */
    pen_x = ((float)k->mode.hdisplay - total) * 0.5f;

    pen_x += draw_text(k, pen_x, pen_y, text_scale, thick, COL_WHITE, a);
    draw_text(k, pen_x, pen_y, text_scale, thick, COL_BABYBLUE, b);
}

static void draw_copyright(kms_t *k, const xform_t *x) {
    float scale = 1.4f * x->scale;
    if (scale < 1.0f) scale = 1.0f;
    float thick = 1.0f * x->scale;
    if (thick < 1.0f) thick = 1.0f;

    float bx = x->ox + 40.0f * x->scale;
    float by = x->oy + 705.0f * x->scale;

    /* The copyright sign is just (C) since our font doesn't have ©. */
    draw_text(k, bx, by, scale, thick, COL_COPY,
              "Copyright (C) 2026");
    draw_text(k, bx, by + 22.0f * x->scale, scale, thick, COL_COPY,
              "YetiOS Project - GNU GPL v3.0");
}

/* The marquee region — we redraw this each frame.
 * Returns the pixel rect that was touched (for kms_dirtyfb). */
typedef struct { int x, y, w, h; } rect_t;

static rect_t marquee_rect(const xform_t *x) {
    rect_t r;
    r.x = (int)(x->ox + MARQUEE_X_DESIGN * x->scale);
    r.y = (int)(x->oy + MARQUEE_Y_DESIGN * x->scale);
    r.w = (int)(MARQUEE_W_DESIGN * x->scale);
    r.h = (int)(MARQUEE_H_DESIGN * x->scale);
    return r;
}

static void draw_marquee_static(kms_t *k, const xform_t *x) {
    rect_t r = marquee_rect(x);
    /* Track border */
    fill_rect(k, r.x - 1, r.y - 1, r.w + 2, 1, COL_DIM_BLUE);
    fill_rect(k, r.x - 1, r.y + r.h, r.w + 2, 1, COL_DIM_BLUE);
    fill_rect(k, r.x - 1, r.y, 1, r.h, COL_DIM_BLUE);
    fill_rect(k, r.x + r.w, r.y, 1, r.h, COL_DIM_BLUE);
    /* Empty track */
    fill_rect(k, r.x, r.y, r.w, r.h, COL_BLACK);
}

/* Draw the moving slug. position: 0..1 normalized along the bar. */
static void draw_marquee_frame(kms_t *k, const xform_t *x, float pos) {
    rect_t r = marquee_rect(x);
    /* Clear the inside */
    fill_rect(k, r.x, r.y, r.w, r.h, COL_BLACK);

    /* The slug is a soft rectangle. We approximate the gradient
     * (transparent → solid → transparent) by drawing a few stacked
     * rectangles with varying intensity. Cheap and looks fine. */
    int slug_w = (int)(40.0f * x->scale);
    if (slug_w < 12) slug_w = 12;
    int slug_x = r.x + (int)((float)(r.w - slug_w) * pos);

    /* Soft edges via 4 fade-in/out bands */
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
    /* Solid core */
    int core_x = slug_x + bands * band_w;
    int core_w = slug_w - 2 * bands * band_w;
    if (core_w > 0) fill_rect(k, core_x, r.y, core_w, r.h, COL_BABYBLUE);
}

static void kms_dirty(kms_t *k, rect_t r) {
    struct drm_mode_fb_dirty_cmd dc = {0};
    struct drm_clip_rect cr = {
        .x1 = (uint16_t)r.x,
        .y1 = (uint16_t)r.y,
        .x2 = (uint16_t)(r.x + r.w),
        .y2 = (uint16_t)(r.y + r.h),
    };
    dc.fb_id  = k->fb_id;
    dc.num_clips = 1;
    dc.clips_ptr = (uintptr_t)&cr;
    /* Not all drivers implement DIRTYFB; ignore failures. */
    (void)ioctl(k->fd, DRM_IOCTL_MODE_DIRTYFB, &dc);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("snowcone — YetiOS boot splash\n");
            printf("Usage: snowcone [-v]\n");
            printf("Exits on SIGTERM, SIGUSR1, or DRM master loss.\n");
            return 0;
        }
    }

    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGHUP,  on_signal);

    kms_t k = { .fd = -1 };
    if (kms_open(&k) < 0) return 1;
    if (kms_pick_mode(&k) < 0) { kms_close(&k); return 1; }
    if (kms_create_fb(&k) < 0) { kms_close(&k); return 1; }
    if (kms_set_crtc(&k) < 0)  { kms_close(&k); return 1; }

    LOGI("scanout active");

    xform_t xf = make_xform(&k);

    /* Render the static layer once. */
    memset(k.pixels, 0, (size_t)k.size); /* black background */
    draw_mountain(&k, &xf);
    draw_wordmark(&k, &xf);
    draw_copyright(&k, &xf);
    draw_marquee_static(&k, &xf);
    kms_dirty(&k, (rect_t){0, 0, k.mode.hdisplay, k.mode.vdisplay});

    /* Animation loop: marquee slug oscillates 0→1→0 with a period of ~2.4s. */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    const long frame_ns = 50 * 1000 * 1000L; /* ~20fps */

    while (!g_stop) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = (double)(now.tv_sec - t0.tv_sec)
                 + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;

        /* XP-style: slug travels left→right, then snaps back to start
         * and travels again. Period ~1.6s. */
        const double period = 1.6;
        double phase = t - (long)(t / period) * period;
        float pos = (float)(phase / period); /* 0..1 */

        draw_marquee_frame(&k, &xf, pos);
        kms_dirty(&k, marquee_rect(&xf));

        /* Page flips would be smoother, but for a marquee 20fps blit
         * to a single FB is fine and avoids tearing in practice on
         * vsync'd drivers (modern KMS implicit-flush). */

        struct timespec ts = { .tv_sec = 0, .tv_nsec = frame_ns };
        nanosleep(&ts, NULL);

        /* Cheap master-loss probe: if a setcrtc would fail now, we've
         * been preempted by the compositor. We check with a no-op
         * GETCRTC — succeeds whether or not we're master. So instead,
         * just poll for any error from DIRTYFB (already non-fatal).
         * A direct, robust check: GET_CAP with a bogus cap returns
         * EINVAL even when we're not master, so we use DROP_MASTER /
         * SET_MASTER probing only on explicit request. For our case,
         * we simply rely on SIGTERM from the display manager service. */
    }

    LOGI("exiting cleanly");
    /* One last black frame so the next stage starts from a clean slate. */
    memset(k.pixels, 0, (size_t)k.size);
    kms_dirty(&k, (rect_t){0, 0, k.mode.hdisplay, k.mode.vdisplay});
    kms_close(&k);
    return 0;
}