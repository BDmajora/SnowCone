# snowcone - from-scratch boot splash for YetiOS.
# The default renderer uses Linux DRM/KMS. The FreeBSD target uses vt/fb.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11
LDFLAGS ?=

BIN     := snowcone
SRCS    := main.c src/sc_kms.c src/sc_raster.c src/sc_font.c src/sc_theme.c
OBJS    := $(SRCS:.c=.o)

FREEBSD_BIN  := snowcone-freebsd
FREEBSD_SRCS := main_freebsd.c src/sc_fb_freebsd.c src/sc_raster.c src/sc_font.c src/sc_theme.c

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

clean:
	rm -f $(BIN) $(OBJS) $(FREEBSD_BIN)

install: $(BIN)
	install -d $(DESTDIR)/usr/sbin
	install -m 0755 $(BIN) $(DESTDIR)/usr/sbin/$(BIN)
	install -d $(DESTDIR)/etc/init.d
	install -m 0755 snowcone.openrc $(DESTDIR)/etc/init.d/snowcone

freebsd: $(FREEBSD_BIN)

$(FREEBSD_BIN): $(FREEBSD_SRCS)
	$(CC) $(CFLAGS) -DSNOWCONE_BACKEND_FREEBSD -Iinclude -o $@ $(FREEBSD_SRCS) $(LDFLAGS)

install-freebsd: $(FREEBSD_BIN)
	install -d $(DESTDIR)/usr/local/sbin
	install -m 0755 $(FREEBSD_BIN) $(DESTDIR)/usr/local/sbin/snowcone
	install -d $(DESTDIR)/etc/rc.d
	install -m 0755 snowcone.freebsd.rc $(DESTDIR)/etc/rc.d/yetios_snowcone
	install -m 0755 snowcone.freebsd-handoff.rc $(DESTDIR)/etc/rc.d/yetios_snowcone_handoff

.PHONY: all clean install freebsd install-freebsd
