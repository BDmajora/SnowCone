# snowcone — from-scratch boot splash for YetiOS.
# No library deps: just libc and Linux kernel DRM headers.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11
LDFLAGS ?=

BIN := snowcone

all: $(BIN)

$(BIN): snowcone.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)

install: $(BIN)
	install -d $(DESTDIR)/usr/sbin
	install -m 0755 $(BIN) $(DESTDIR)/usr/sbin/$(BIN)
	install -d $(DESTDIR)/etc/init.d
	install -m 0755 snowcone.openrc $(DESTDIR)/etc/init.d/snowcone

.PHONY: all clean install