#ifndef ADDR_ALLOC_H
#define ADDR_ALLOC_H

#include <stdint.h>

/*
 * Minimal GPU virtual address allocator for softpin.
 *
 * The IGT allocator exists because hardcoded addresses collide with the
 * display server's existing VM mappings and because the safe start
 * offset is platform-dependent. This is the single-process, bump-only
 * subset: it queries the kernel for the safe start, then hands out
 * page-aligned offsets monotonically. It never reuses an address, which
 * is fine for short-lived blit operations.
 */

typedef struct addr_alloc addr_alloc_t;

addr_alloc_t *addr_alloc_open(int drm_fd);
void          addr_alloc_close(addr_alloc_t *a);
uint64_t      addr_alloc_get(addr_alloc_t *a, uint32_t size);
uint64_t      addr_alloc_range_start(addr_alloc_t *a);

#endif
