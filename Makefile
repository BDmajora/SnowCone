# snowcone — from-scratch boot splash for YetiOS.
# No library deps: just libc and Linux kernel DRM headers.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11
LDFLAGS ?=

BIN     := snowcone
SRCS    := main.c src/sc_kms.c src/sc_raster.c src/sc_font.c src/sc_theme.c
OBJS    := $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

clean:
	rm -f $(BIN) $(OBJS)

install: $(BIN)
	install -d $(DESTDIR)/usr/sbin
	install -m 0755 $(BIN) $(DESTDIR)/usr/sbin/$(BIN)
	install -d $(DESTDIR)/etc/init.d
	install -m 0755 snowcone.openrc $(DESTDIR)/etc/init.d/snowcone

.PHONY: all clean install