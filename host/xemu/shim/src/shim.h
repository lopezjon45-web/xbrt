#pragma once

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "system/memory.h"

extern const XbrtHost *g_xbrt_host;

typedef struct XbrtDeviceSpec {
    const char *device;
    void (*init)(PCIBus *bus, int devfn, MemoryRegion *ram);
    int devfn;
} XbrtDeviceSpec;

unsigned xbrt_machine_create(const XbrtHost *host, const XbrtDeviceSpec *devices, unsigned count, XbrtBar *bars,
                             unsigned max_bars);

unsigned xbrt_pci_device_count(void);
PCIDevice *xbrt_pci_device(unsigned i);
void xbrt_qom_reset_all(void);
void xbrt_runstate_start(void);

extern MemoryRegion xbrt_board_ram;
void xbrt_memory_add_bar(PCIDevice *dev, int index, MemoryRegion *root);
unsigned xbrt_memory_freeze(const XbrtDeviceSpec *devices, XbrtBar *bars, unsigned max_bars);

void xbrt_clock_init(void);
void xbrt_count_vblank(void);
void xbrt_clock_start(void);
void xbrt_bql_release_if_held(void);
void xbrt_thread_set_affinity(void *thread);
