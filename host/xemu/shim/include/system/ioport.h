#ifndef SYSTEM_IOPORT_H
#define SYSTEM_IOPORT_H

#include "qemu/osdep.h"

typedef struct MemoryRegionPortio {
    uint32_t offset;
    uint32_t len;
    unsigned size;
    uint32_t (*read)(void *opaque, uint32_t address);
    void (*write)(void *opaque, uint32_t address, uint32_t data);
} MemoryRegionPortio;

#define PORTIO_END_OF_LIST() {0}

typedef struct PortioList {
    const MemoryRegionPortio *ports;
    Object *owner;
    MemoryRegion *address_space;
    uint32_t addr;
    unsigned nr;
    MemoryRegion **regions;
    void *opaque;
    const char *name;
    bool flush_coalesced_mmio;
} PortioList;

void portio_list_init(PortioList *piolist, Object *owner, const MemoryRegionPortio *callbacks, void *opaque,
                      const char *name);
void portio_list_set_flush_coalesced(PortioList *piolist);
void portio_list_add(PortioList *piolist, MemoryRegion *address_space, uint32_t addr);

#endif
