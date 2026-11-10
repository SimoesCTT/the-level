CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += -Iinclude -Isrc \
           -DCL_TARGET_OPENCL_VERSION=300 \
           $(shell pkg-config --cflags libdrm 2>/dev/null) \
           $(shell pkg-config --cflags OpenCL 2>/dev/null)
LDFLAGS ?=
LDFLAGS += $(shell pkg-config --libs libdrm 2>/dev/null) \
           $(shell pkg-config --libs OpenCL 2>/dev/null)

TEST := tests/test_blit
SRC  := src/blit.c src/blit_cpu.c src/blit_cl.c src/blit_i915.c \
        src/addr_alloc.c tests/test_blit.c

all: $(TEST)

$(TEST): $(SRC) include/blit.h src/blit_backend.h src/blit_cl.h \
         src/blit_i915.h src/addr_alloc.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

check: $(TEST)
	RUSTICL_ENABLE=iris ./$(TEST)

clean:
	rm -f $(TEST)

.PHONY: all check clean
