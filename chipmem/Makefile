CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += -Iinclude -I../blit/include -I../blit/src \
           -DCL_TARGET_OPENCL_VERSION=300 \
           $(shell pkg-config --cflags libdrm 2>/dev/null) \
           $(shell pkg-config --cflags OpenCL 2>/dev/null)
LDFLAGS ?=
LDFLAGS += $(shell pkg-config --libs libdrm 2>/dev/null) \
           $(shell pkg-config --libs OpenCL 2>/dev/null)

TEST    := tests/test_coherency
COUPLED := tests/test_coupled

all: $(TEST) $(COUPLED)

$(TEST): src/chipmem.c tests/test_coherency.c include/chipmem.h
	$(CC) $(CFLAGS) -o $@ src/chipmem.c tests/test_coherency.c $(LDFLAGS) -lm

$(COUPLED): src/chipmem.c ../blit/src/blit.c ../blit/src/blit_cpu.c \
            ../blit/src/blit_i915.c ../blit/src/blit_cl.c \
            ../blit/src/addr_alloc.c tests/test_coupled.c \
            include/chipmem.h ../blit/include/blit.h
	$(CC) $(CFLAGS) -o $@ src/chipmem.c ../blit/src/blit.c \
	   ../blit/src/blit_cpu.c ../blit/src/blit_i915.c \
	   ../blit/src/blit_cl.c ../blit/src/addr_alloc.c \
	   tests/test_coupled.c $(LDFLAGS) -lm

check: $(TEST)
	./$(TEST); rc=$$?; test $$rc -eq 0 -o $$rc -eq 77

check-coupling: $(COUPLED)
	RUSTICL_ENABLE=iris ./$(COUPLED)

clean:
	rm -f $(TEST) $(COUPLED)

.PHONY: all check check-coupling clean
