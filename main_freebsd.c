/*
 * snowcone/main_freebsd.c - FreeBSD framebuffer boot splash entry point.
 *
 * This uses the same SnowCone scene as the Linux DRM renderer, but keeps the
 * FreeBSD console in graphics mode until the rc system stops us near login.
 */

#include "sc_kms.h"
#include "sc_log.h"
#include "sc_scene.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int g_verbose = 0;

static volatile sig_atomic_t g_stop = 0;

static void
on_signal(int s)
{
    (void)s;
    g_stop = 1;
}

static void
sleep_frame(long ns)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = ns;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR && !g_stop)
        ;
}

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("snowcone - YetiOS boot splash\n");
            printf("Usage: snowcone [-v]\n");
            printf("Exits on SIGTERM, SIGUSR1, SIGINT, or SIGHUP.\n");
            return 0;
        }
    }

    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGHUP,  on_signal);

    kms_t k = { .fd = -1, .tty_fd = -1 };
    if (kms_open(&k) < 0)
        return 1;
    if (kms_pick_mode(&k) < 0) {
        kms_close(&k);
        return 1;
    }
    if (kms_create_fb(&k) < 0) {
        kms_close(&k);
        return 1;
    }
    if (kms_set_crtc(&k) < 0) {
        kms_close(&k);
        return 1;
    }

    LOGI("FreeBSD framebuffer active");

    xform_t xf = sc_make_xform(&k);
    sc_draw_static(&k, &xf);
    kms_dirty(&k, (rect_t){0, 0, (int)k.mode.hdisplay, (int)k.mode.vdisplay});

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    const long frame_ns = 50 * 1000 * 1000L;
    while (!g_stop) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = (double)(now.tv_sec - t0.tv_sec)
                 + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;

        const double period = 1.6;
        double phase = t - (long)(t / period) * period;
        float pos = (float)(phase / period);

        sc_draw_static(&k, &xf);
        sc_draw_marquee_frame(&k, &xf, pos);
        kms_dirty(&k, (rect_t){0, 0, (int)k.mode.hdisplay, (int)k.mode.vdisplay});
        sleep_frame(frame_ns);
    }

    LOGI("exiting cleanly");
    memset(k.pixels, 0, (size_t)k.size);
    kms_dirty(&k, (rect_t){0, 0, (int)k.mode.hdisplay, (int)k.mode.vdisplay});
    kms_close(&k);
    return 0;
}
