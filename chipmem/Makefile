CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Iinclude -I../blit/include -I../blit/src \
           -DCL_TARGET_OPENCL_VERSION=300 \
           $(shell pkg-config --cflags libdrm 2>/dev/null) \
           $(shell pkg-config --cflags OpenCL 2>/dev/null)
LDFLAGS ?= $(shell pkg-config --libs libdrm 2>/dev/null) \
           $(shell pkg-config --libs OpenCL 2>/dev/null)

TEST        := tests/test_coherency
COUPLED     := tests/test_coupled
SRC         := src/chipmem.c tests/test_coherency.c
COUPLED_SRC := src/chipmem.c ../blit/src/blit_i915.c tests/test_coupled.c

all: $(TEST) $(COUPLED)

$(TEST): $(SRC) include/chipmem.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(COUPLED): $(COUPLED_SRC) include/chipmem.h ../blit/src/blit_i915.h
	$(CC) $(CFLAGS) -o $@ $(COUPLED_SRC) $(LDFLAGS)

check: $(TEST)
	./$(TEST)

check-coupling: $(COUPLED)
	./$(COUPLED)

clean:
	rm -f $(TEST) $(COUPLED)

.PHONY: all check check-coupling clean
