CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra

all:
	$(MAKE) -C blit
	$(MAKE) -C chipmem

check:
	$(MAKE) -C blit check
	$(MAKE) -C chipmem check

check-coupling:
	$(MAKE) -C chipmem check-coupling

clean:
	$(MAKE) -C blit clean
	$(MAKE) -C chipmem clean

.PHONY: all check check-coupling clean
