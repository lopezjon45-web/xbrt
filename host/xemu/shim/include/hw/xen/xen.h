#ifndef HW_XEN_H
#define HW_XEN_H
#include "system/memory.h"
static inline void xen_register_framebuffer(MemoryRegion *mr) { (void)mr; }
#endif
