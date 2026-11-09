#ifndef BLIT_CL_H
#define BLIT_CL_H

#include <CL/cl.h>
#include "blit.h"

int  blit_cl_init(void);
void blit_cl_shutdown(void);
int  blit_cl_available(void);
int  blit_cl_execute(const blit_desc_t *d, cl_event *out_event);

#endif
