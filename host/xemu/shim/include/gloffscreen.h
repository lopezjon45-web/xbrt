#ifndef XBRT_SHIM_GLOFFSCREEN_H
#define XBRT_SHIM_GLOFFSCREEN_H

#include <epoxy/gl.h>

typedef struct GloContext GloContext;
GloContext *glo_context_create(void);
void glo_set_current(GloContext *context);

#endif
