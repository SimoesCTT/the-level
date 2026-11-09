#ifndef BLIT_BACKEND_H
#define BLIT_BACKEND_H

#include "blit.h"

struct blit_backend_ops {
    const char *name;
    int  (*init)(int fd);
    void (*shutdown)(void);
    int  (*submit)(const blit_desc_t *desc, blit_mode_t mode,
                   void **out_handle);
    int  (*wait)(void *handle);
    int  (*done)(void *handle);
    void (*release)(void *handle);
    /* Bitmask of BLIT_CAP_* this backend supports. */
    uint32_t caps;
};

struct blit_op {
    void *handle;
    const struct blit_backend_ops *ops;
};

const char *blit_backend_name(void);

extern const struct blit_backend_ops *blit_backend_i915(void);
extern const struct blit_backend_ops *blit_backend_cl(void);
extern const struct blit_backend_ops *blit_backend_cpu(void);

#endif
