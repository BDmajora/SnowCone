/*
 * snowcone/src/sc_kms.c — DRM/KMS framebuffer management.
 */

#include "sc_kms.h"
#include "sc_log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/types.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

/* ------------------------------------------------------------------ */
/* Device discovery                                                   */
/* ------------------------------------------------------------------ */

static int try_card(const char *path, kms_t *k) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    struct drm_get_cap cap = { .capability = DRM_CAP_DUMB_BUFFER };
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 || !cap.value) {
        close(fd);
        return -1;
    }
    k->fd = fd;
    return 0;
}

int kms_open(kms_t *k) {
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

/* ------------------------------------------------------------------ */
/* Mode selection                                                     */
/* ------------------------------------------------------------------ */

int kms_pick_mode(kms_t *k) {
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

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector gc = {0};
        gc.connector_id = conns[i];

        if (ioctl(k->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) continue;
        if (gc.connection != 1) continue;
        if (gc.count_modes == 0) continue;

        struct drm_mode_modeinfo *modes = calloc(gc.count_modes, sizeof(*modes));
        uint32_t *cencs = calloc(gc.count_encoders ? gc.count_encoders : 1, sizeof(*cencs));
        uint32_t *props = calloc(gc.count_props ? gc.count_props : 1, sizeof(*props));
        uint64_t *pvals = calloc(gc.count_props ? gc.count_props : 1, sizeof(*pvals));
        gc.modes_ptr       = (uintptr_t)modes;
        gc.encoders_ptr    = (uintptr_t)cencs;
        gc.props_ptr       = (uintptr_t)props;
        gc.prop_values_ptr = (uintptr_t)pvals;
        if (ioctl(k->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) {
            free(modes); free(cencs); free(props); free(pvals);
            continue;
        }

        k->mode    = modes[0];
        k->conn_id = conns[i];
        k->enc_id  = gc.encoder_id;

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

/* ------------------------------------------------------------------ */
/* Framebuffer                                                        */
/* ------------------------------------------------------------------ */

int kms_create_fb(kms_t *k) {
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

    memset(p, 0, (size_t)k->size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* CRTC                                                               */
/* ------------------------------------------------------------------ */

int kms_set_crtc(kms_t *k) {
    struct drm_mode_crtc cm = {0};
    cm.crtc_id            = k->crtc_id;
    cm.fb_id              = k->fb_id;
    cm.set_connectors_ptr = (uintptr_t)&k->conn_id;
    cm.count_connectors   = 1;
    cm.mode               = k->mode;
    cm.mode_valid         = 1;
    if (ioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &cm) < 0) {
        LOGE("SETCRTC: %s", strerror(errno));
        return -1;
    }
    return 0;
}

void kms_dirty(kms_t *k, rect_t r) {
    struct drm_mode_fb_dirty_cmd dc = {0};
    struct drm_clip_rect cr = {
        .x1 = (uint16_t)r.x,
        .y1 = (uint16_t)r.y,
        .x2 = (uint16_t)(r.x + r.w),
        .y2 = (uint16_t)(r.y + r.h),
    };
    dc.fb_id     = k->fb_id;
    dc.num_clips = 1;
    dc.clips_ptr = (uintptr_t)&cr;
    (void)ioctl(k->fd, DRM_IOCTL_MODE_DIRTYFB, &dc);
}

void kms_close(kms_t *k) {
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