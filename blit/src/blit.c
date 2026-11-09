#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "blit.h"
#include "blit_backend.h"

static const struct blit_backend_ops *g_ops = NULL;
static int g_initialized = 0;
static int g_shared_fd = -1;

void blit_set_fd(int fd) { g_shared_fd = fd; }

int blit_init(void)
{
    const struct blit_backend_ops *candidates[] = {
        blit_backend_i915(),
        blit_backend_cl(),
        blit_backend_cpu(),
    };
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        const struct blit_backend_ops *ops = candidates[i];
        if (!ops) continue;
        int rc = ops->init(g_shared_fd);
        if (rc == 0) {
            g_ops = ops;
            g_initialized = 1;
            return 0;
        }
    }
    errno = ENODEV;
    return -ENODEV;
}

blit_backend_t blit_backend(void)
{
    if (!g_ops) return BLIT_BACKEND_CPU_FALLBACK;
    if (strcmp(g_ops->name, "i915") == 0)    return BLIT_BACKEND_GPU_ZERO_COPY;
    if (strcmp(g_ops->name, "rusticl") == 0) return BLIT_BACKEND_GPU_COPY;
    return BLIT_BACKEND_CPU_FALLBACK;
}


uint32_t blit_caps(void)
{
    return g_ops ? g_ops->caps : 0;
}

const char *blit_backend_name(void)
{
    return g_ops ? g_ops->name : "none";
}

blit_op_t *blit(const blit_desc_t *desc, blit_mode_t mode)
{
    if (!g_initialized) { errno = EPERM; return NULL; }
    if (!desc || !desc->dst) { errno = EINVAL; return NULL; }

    void *handle = NULL;
    const struct blit_backend_ops *used = g_ops;
    int rc = used->submit(desc, mode, &handle);
    if (rc == -ENOTSUP) {
        used = blit_backend_cpu();
        rc = used->submit(desc, mode, &handle);
    }
    if (rc != 0) { errno = -rc; return NULL; }

    blit_op_t *op = calloc(1, sizeof(*op));
    if (!op) { used->release(handle); return NULL; }
    op->handle = handle;
    op->ops    = used;
    return op;
}

int blit_op_wait(blit_op_t *op)
{
    if (!op) { errno = EINVAL; return -EINVAL; }
    int rc = op->ops->wait(op->handle);
    free(op);
    return rc;
}

int blit_op_done(blit_op_t *op)
{
    if (!op) { errno = EINVAL; return -EINVAL; }
    return op->ops->done(op->handle);
}

void blit_op_release(blit_op_t *op)
{
    if (!op) return;
    op->ops->release(op->handle);
    free(op);
}

void blit_shutdown(void)
{
    if (g_ops) g_ops->shutdown();
    g_ops = NULL;
    g_initialized = 0;
}
