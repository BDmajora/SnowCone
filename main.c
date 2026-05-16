/*
 * snowcone/main.c — Boot splash entry point.
 *
 * Lifecycle:
 *   1. Open DRM, pick mode, create framebuffer, set CRTC.
 *   2. Render the static scene (logo, wordmark, copyright, marquee track).
 *   3. Animation loop: redraw the marquee slug each frame.
 *      Every ~200ms, poll for DRM master loss — if snowfall (or anything
 *      else) has grabbed master, exit cleanly so the new client can take
 *      over without fighting us for the framebuffer.
 *   4. On SIGTERM/SIGUSR1/SIGINT/SIGHUP or master loss: tear down, exit.
 */

#include "sc_kms.h"
#include "sc_log.h"
#include "sc_scene.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#include <drm/drm.h>

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

int g_verbose = 0;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* ------------------------------------------------------------------ */
/* Master-loss detection                                              */
/* ------------------------------------------------------------------ */

/*
 * Returns 1 if we still hold DRM master, 0 if we've lost it.
 *
 * We use DRM_IOCTL_GET_MAGIC as a cheap master-presence probe: it
 * succeeds for the current master and fails with EACCES / EPERM
 * otherwise. (Calling drmSetMaster repeatedly would also work but
 * has side effects on some kernels; this is read-only.)
 *
 * On any unexpected error we conservatively assume master is still
 * ours — better a too-long splash than a flickering early exit.
 */
static int still_have_master(int fd) {
    struct drm_auth a = { .magic = 0 };
    if (ioctl(fd, DRM_IOCTL_GET_MAGIC, &a) == 0) return 1;
    if (errno == EACCES || errno == EPERM) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("snowcone — YetiOS boot splash\n");
            printf("Usage: snowcone [-v]\n");
            printf("Exits on SIGTERM, SIGUSR1, SIGINT, SIGHUP, or DRM master loss.\n");
            return 0;
        }
    }

    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGHUP,  on_signal);

    kms_t k = { .fd = -1 };
    if (kms_open(&k) < 0)         return 1;
    if (kms_pick_mode(&k) < 0)  { kms_close(&k); return 1; }
    if (kms_create_fb(&k) < 0)  { kms_close(&k); return 1; }
    if (kms_set_crtc(&k) < 0)   { kms_close(&k); return 1; }

    LOGI("scanout active");

    xform_t xf = sc_make_xform(&k);

    /* Render the static layer once. */
    sc_draw_static(&k, &xf);
    kms_dirty(&k, (rect_t){0, 0, k.mode.hdisplay, k.mode.vdisplay});

    /* Animation loop. */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    const long frame_ns = 50 * 1000 * 1000L; /* ~20 fps */

    /* Check master every Nth frame (~200ms at 20fps). The probe is
     * cheap but there's no point hammering it every frame. */
    const int master_check_interval = 4;
    int frame_count = 0;
    int lost_master = 0;

    while (!g_stop) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = (double)(now.tv_sec - t0.tv_sec)
                 + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;

        const double period = 1.6;
        double phase = t - (long)(t / period) * period;
        float pos = (float)(phase / period);

        sc_draw_marquee_frame(&k, &xf, pos);
        kms_dirty(&k, sc_marquee_rect(&xf));

        if (++frame_count >= master_check_interval) {
            frame_count = 0;
            if (!still_have_master(k.fd)) {
                LOGI("DRM master taken by another client, exiting");
                lost_master = 1;
                break;
            }
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = frame_ns };
        nanosleep(&ts, NULL);
    }

    /*
     * Teardown.
     *
     * If we lost master, do NOT wipe the framebuffer or flush dirty
     * rects — the new master (snowfall) is already painting, and
     * those ioctls would either no-op (we're not master anymore) or,
     * worse, race with snowfall's first frame and produce a black
     * flash. Just release our resources and exit.
     *
     * kms_close handles munmap / RMFB / DESTROY_DUMB / close; the
     * RMFB and DESTROY_DUMB calls will fail without master but that's
     * fine — closing the fd releases everything server-side anyway
     * via the kernel's DRM_RELEASE path.
     */
    if (lost_master) {
        LOGI("exiting (master loss)");
        kms_close(&k);
        return 0;
    }

    LOGI("exiting cleanly");
    memset(k.pixels, 0, (size_t)k.size);
    kms_dirty(&k, (rect_t){0, 0, k.mode.hdisplay, k.mode.vdisplay});
    kms_close(&k);
    return 0;
}