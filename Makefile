CC ?= cc
CFLAGS ?= -O2 -fPIC -Wall -Wextra -std=c11
LDFLAGS_SO ?= -shared -ldl
PREFIX ?= /usr/local

all: libnvidia-hide.so nvidia-hide

libnvidia-hide.so: libnvidia-hide.c
	$(CC) $(CFLAGS) $(LDFLAGS_SO) -o $@ $<

nvidia-hide: nvidia-hide.c
	$(CC) -O2 -Wall -Wextra -std=c11 -o $@ $<

install:
	install -Dm755 nvidia-hide $(DESTDIR)$(PREFIX)/bin/nvidia-hide
	install -Dm755 libnvidia-hide.so $(DESTDIR)$(PREFIX)/lib/libnvidia-hide.so

clean:
	rm -f libnvidia-hide.so nvidia-hide
