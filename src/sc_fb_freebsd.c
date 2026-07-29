/*
 * snowcone/src/sc_fb_freebsd.c - FreeBSD framebuffer backend.
 */

#ifdef SNOWCONE_BACKEND_FREEBSD

#include "sc_kms.h"
#include "sc_log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/consio.h>
#include <sys/fbio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

static const char *const FB_PATHS[] = {
    "/dev/fb0",
    "/dev/fb",
    "/dev/ttyv0",
    "/dev/console",
    NULL,
};

static const char *const TTY_PATHS[] = {
    "/dev/ttyv0",
    "/dev/console",
    NULL,
};

static int
open_console_graphics(kms_t *k)
{
    for (int i = 0; TTY_PATHS[i] != NULL; i++) {
        int fd = open(TTY_PATHS[i], O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        k->tty_fd = fd;
        k->old_kd_mode = -1;
        if (ioctl(fd, KDGETMODE, &k->old_kd_mode) < 0)
            k->old_kd_mode = -1;

        if (ioctl(fd, KDSETMODE, KD_GRAPHICS) < 0) {
            LOGE("KDSETMODE graphics on %s: %s", TTY_PATHS[i], strerror(errno));
            close(fd);
            k->tty_fd = -1;
            continue;
        }

        LOGV("console graphics mode on %s", TTY_PATHS[i]);
        return 0;
    }

    LOGE("could not put a FreeBSD console into graphics mode");
    return -1;
}

static int
open_framebuffer(kms_t *k, struct fbtype *fb)
{
    for (int i = 0; FB_PATHS[i] != NULL; i++) {
        int fd = open(FB_PATHS[i], O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        memset(fb, 0, sizeof(*fb));
        if (ioctl(fd, FBIOGTYPE, fb) == 0 &&
            fb->fb_width > 0 &&
            fb->fb_height > 0 &&
            fb->fb_size > 0) {
            k->fd = fd;
            LOGV("opened framebuffer %s", FB_PATHS[i]);
            return 0;
        }

        close(fd);
    }

    LOGE("no usable FreeBSD framebuffer found");
    return -1;
}

static void
load_rgb_offsets(kms_t *k)
{
    struct fb_rgboffs offs;

    k->red_shift = 16;
    k->green_shift = 8;
    k->blue_shift = 0;

    memset(&offs, 0, sizeof(offs));
    if (ioctl(k->fd, FBIO_GETRGBOFFS, &offs) == 0) {
        k->red_shift = offs.red;
        k->green_shift = offs.green;
        k->blue_shift = offs.blue;
    }
}

int
kms_open(kms_t *k)
{
    struct fbtype fb;
    u_int line_width = 0;
    int bytes_per_pixel;

    memset(k, 0, sizeof(*k));
    k->fd = -1;
    k->tty_fd = -1;
    k->old_kd_mode = -1;
    k->front = MAP_FAILED;

    if (open_console_graphics(k) < 0)
        return -1;

    if (open_framebuffer(k, &fb) < 0) {
        kms_close(k);
        return -1;
    }

    if (fb.fb_depth != 32 && fb.fb_depth != 24 && fb.fb_depth != 16) {
        LOGE("unsupported framebuffer depth: %d", fb.fb_depth);
        kms_close(k);
        return -1;
    }

    bytes_per_pixel = fb.fb_depth / 8;
    k->mode.hdisplay = (uint32_t)fb.fb_width;
    k->mode.vdisplay = (uint32_t)fb.fb_height;
    k->mode.vrefresh = 60;
    k->front_depth = fb.fb_depth;
    k->front_size = (uint64_t)fb.fb_size;
    if (ioctl(k->fd, FBIO_GETLINEWIDTH, &line_width) == 0 && line_width > 0)
        k->front_pitch = (uint32_t)line_width;
    else
        k->front_pitch = (uint32_t)(fb.fb_size / fb.fb_height);
    if (k->front_pitch < (uint32_t)(fb.fb_width * bytes_per_pixel))
        k->front_pitch = (uint32_t)(fb.fb_width * bytes_per_pixel);

    load_rgb_offsets(k);

    k->front = mmap(NULL, (size_t)k->front_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, k->fd, 0);
    if (k->front == MAP_FAILED) {
        LOGE("framebuffer mmap: %s", strerror(errno));
        kms_close(k);
        return -1;
    }

    LOGI("display %ux%u depth %d",
         k->mode.hdisplay, k->mode.vdisplay, k->front_depth);
    return 0;
}

int
kms_pick_mode(kms_t *k)
{
    return k->mode.hdisplay > 0 && k->mode.vdisplay > 0 ? 0 : -1;
}

int
kms_create_fb(kms_t *k)
{
    uint64_t size;

    k->pitch = k->mode.hdisplay * 4;
    size = (uint64_t)k->pitch * (uint64_t)k->mode.vdisplay;
    if (size == 0 || size > (uint64_t)SIZE_MAX) {
        LOGE("invalid backbuffer size");
        return -1;
    }

    k->size = size;
    k->pixels = calloc(1, (size_t)k->size);
    if (k->pixels == NULL) {
        LOGE("backbuffer allocation failed");
        return -1;
    }

    return 0;
}

int
kms_set_crtc(kms_t *k)
{
    (void)k;
    return 0;
}

static uint32_t
pack32(const kms_t *k, uint32_t argb)
{
    uint32_t r = (argb >> 16) & 0xffu;
    uint32_t g = (argb >> 8) & 0xffu;
    uint32_t b = argb & 0xffu;

    return (r << k->red_shift) | (g << k->green_shift) | (b << k->blue_shift);
}

static uint16_t
pack16(const kms_t *k, uint32_t argb)
{
    uint32_t r = (argb >> 16) & 0xffu;
    uint32_t g = (argb >> 8) & 0xffu;
    uint32_t b = argb & 0xffu;

    return (uint16_t)(((r >> 3) << k->red_shift) |
                      ((g >> 2) << k->green_shift) |
                      ((b >> 3) << k->blue_shift));
}

static void
put24(const kms_t *k, unsigned char *dst, uint32_t argb)
{
    int ri = k->red_shift / 8;
    int gi = k->green_shift / 8;
    int bi = k->blue_shift / 8;

    if (ri < 0 || ri > 2 || gi < 0 || gi > 2 || bi < 0 || bi > 2) {
        ri = 2;
        gi = 1;
        bi = 0;
    }

    dst[ri] = (unsigned char)((argb >> 16) & 0xffu);
    dst[gi] = (unsigned char)((argb >> 8) & 0xffu);
    dst[bi] = (unsigned char)(argb & 0xffu);
}

void
kms_dirty(kms_t *k, rect_t r)
{
    int x0 = r.x;
    int y0 = r.y;
    int x1 = r.x + r.w;
    int y1 = r.y + r.h;
    int src_stride = (int)(k->pitch / 4);
    int bytes_per_pixel;

    if (k->front == MAP_FAILED || k->pixels == NULL)
        return;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int)k->mode.hdisplay)
        x1 = (int)k->mode.hdisplay;
    if (y1 > (int)k->mode.vdisplay)
        y1 = (int)k->mode.vdisplay;
    if (x1 <= x0 || y1 <= y0)
        return;

    bytes_per_pixel = k->front_depth / 8;
    for (int y = y0; y < y1; y++) {
        uint32_t *src = &k->pixels[y * src_stride + x0];
        unsigned char *dst = (unsigned char *)k->front
                           + (uint64_t)y * k->front_pitch
                           + (uint64_t)x0 * bytes_per_pixel;

        for (int x = x0; x < x1; x++) {
            uint32_t argb = *src++;

            if (k->front_depth == 32) {
                *(uint32_t *)dst = pack32(k, argb);
            } else if (k->front_depth == 24) {
                put24(k, dst, argb);
            } else {
                *(uint16_t *)dst = pack16(k, argb);
            }

            dst += bytes_per_pixel;
        }
    }

    (void)msync(k->front, (size_t)k->front_size, MS_ASYNC);
}

void
kms_close(kms_t *k)
{
    if (k->front != NULL && k->front != MAP_FAILED)
        munmap(k->front, (size_t)k->front_size);
    if (k->pixels != NULL)
        free(k->pixels);
    if (k->fd >= 0)
        close(k->fd);
    if (k->tty_fd >= 0) {
        int mode = k->old_kd_mode >= 0 ? k->old_kd_mode : KD_TEXT;
        (void)ioctl(k->tty_fd, KDSETMODE, mode);
        close(k->tty_fd);
    }
}

#endif /* SNOWCONE_BACKEND_FREEBSD */
