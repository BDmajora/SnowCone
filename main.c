/*
 * snowcone/main.c — Boot splash entry point.
 *
 * Lifecycle:
 *   1. Open DRM, pick mode, create framebuffer, set CRTC.
 *   2. Render the static scene (logo, wordmark, copyright, marquee track).
 *   3. Animation loop: redraw the marquee slug each frame.
 *   4. On SIGTERM/SIGUSR1/SIGINT: wipe to black, tear down, exit.
 */

#include "sc_kms.h"
#include "sc_log.h"
#include "sc_scene.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

int g_verbose = 0;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

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
            printf("Exits on SIGTERM, SIGUSR1, or DRM master loss.\n");
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

        struct timespec ts = { .tv_sec = 0, .tv_nsec = frame_ns };
        nanosleep(&ts, NULL);
    }

    LOGI("exiting cleanly");
    memset(k.pixels, 0, (size_t)k.size);
    kms_dirty(&k, (rect_t){0, 0, k.mode.hdisplay, k.mode.vdisplay});
    kms_close(&k);
    return 0;
}