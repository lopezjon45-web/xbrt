#ifndef SYSTEM_MEMORY_H
#define SYSTEM_MEMORY_H

#include "qemu/osdep.h"

enum device_endian { DEVICE_NATIVE_ENDIAN, DEVICE_BIG_ENDIAN, DEVICE_LITTLE_ENDIAN };

typedef struct MemoryRegionAccess {
    unsigned min_access_size;
    unsigned max_access_size;
    bool unaligned;
} MemoryRegionAccess;

struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned size);
    void (*write)(void *opaque, hwaddr addr, uint64_t data, unsigned size);
    enum device_endian endianness;
    MemoryRegionAccess valid;
    MemoryRegionAccess impl;
};

typedef enum MemoryRegionKind { MR_CONTAINER, MR_IO, MR_RAM, MR_ALIAS } MemoryRegionKind;

#define XBRT_MAX_SUBREGIONS 24

struct MemoryRegion {
    MemoryRegionKind kind;
    const char *name;
    Object *owner;
    uint64_t size;
    const MemoryRegionOps *ops;
    void *opaque;
    uint8_t *ram;
    bool ram_owned;
    MemoryRegion *alias;
    hwaddr alias_offset;
    MemoryRegion *container;
    hwaddr addr;
    MemoryRegion *sub[XBRT_MAX_SUBREGIONS];
    unsigned nsub;
    uint8_t dirty_log_mask;
};

enum {
    DIRTY_MEMORY_VGA,
    DIRTY_MEMORY_CODE,
    DIRTY_MEMORY_MIGRATION,
    DIRTY_MEMORY_NV2A,
    DIRTY_MEMORY_NV2A_TEX,
    DIRTY_MEMORY_NUM
};

typedef struct DirtyBitmapSnapshot DirtyBitmapSnapshot;

void memory_region_init(MemoryRegion *mr, Object *owner, const char *name, uint64_t size);
void memory_region_init_io(MemoryRegion *mr, Object *owner, const MemoryRegionOps *ops, void *opaque,
                           const char *name, uint64_t size);
void memory_region_init_ram(MemoryRegion *mr, Object *owner, const char *name, uint64_t size, Error **errp);
void memory_region_init_ram_nomigrate(MemoryRegion *mr, Object *owner, const char *name, uint64_t size,
                                      Error **errp);
void memory_region_init_alias(MemoryRegion *mr, Object *owner, const char *name, MemoryRegion *orig,
                              hwaddr offset, uint64_t size);
void memory_region_add_subregion(MemoryRegion *mr, hwaddr offset, MemoryRegion *subregion);
void memory_region_add_subregion_overlap(MemoryRegion *mr, hwaddr offset, MemoryRegion *subregion, int priority);
void memory_region_del_subregion(MemoryRegion *mr, MemoryRegion *subregion);
void memory_region_destroy(MemoryRegion *mr);
uint64_t memory_region_size(MemoryRegion *mr);
void *memory_region_get_ram_ptr(MemoryRegion *mr);
Object *memory_region_owner(MemoryRegion *mr);
void memory_region_set_log(MemoryRegion *mr, bool log, unsigned client);
void memory_region_set_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size);
bool memory_region_test_and_clear_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client);
void memory_region_set_coalescing(MemoryRegion *mr);
void memory_region_set_flush_coalesced(MemoryRegion *mr);
DirtyBitmapSnapshot *memory_region_snapshot_and_clear_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size,
                                                            unsigned client);
bool memory_region_snapshot_get_dirty(MemoryRegion *mr, DirtyBitmapSnapshot *snap, hwaddr addr, hwaddr size);
void qemu_flush_coalesced_mmio_buffer(void);
void *qemu_ram_block_by_name(const char *name);
void vmstate_register_ram(MemoryRegion *mr, DeviceState *dev);

extern AddressSpace address_space_memory;
uint32_t ldub_phys(AddressSpace *as, hwaddr addr);
uint32_t lduw_le_phys(AddressSpace *as, hwaddr addr);
uint32_t ldl_le_phys(AddressSpace *as, hwaddr addr);
uint64_t ldq_le_phys(AddressSpace *as, hwaddr addr);
void stb_phys(AddressSpace *as, hwaddr addr, uint32_t val);
void stw_le_phys(AddressSpace *as, hwaddr addr, uint32_t val);
void stl_le_phys(AddressSpace *as, hwaddr addr, uint32_t val);
void stq_le_phys(AddressSpace *as, hwaddr addr, uint64_t val);

#endif
