#include <windows.h>

#include "shim.h"
#include "qemu/main-loop.h"

typedef struct FlatRange {
    uint32_t start, end;
    MemoryRegion *mr;
    uint32_t mr_offset;
} FlatRange;

#define MAX_BARS 8
#define MAX_RANGES 32

typedef struct Bar {
    MemoryRegion *root;
    PCIDevice *dev;
    int index;
    const char *device;
    FlatRange ranges[MAX_RANGES];
    unsigned count;
} Bar;

static Bar g_bars[MAX_BARS];
static unsigned g_bar_count;
static bool g_frozen;

MemoryRegion xbrt_board_ram;
struct AddressSpace {
    int unused;
};
AddressSpace address_space_memory;

static void check_not_frozen(const char *what, const char *name)
{
    if (g_frozen)
        xbrt_refuse(XBRT_REFUSE_FEATURE, "%s on \"%s\" after the memory maps were frozen", what, name ? name : "?");
}

static void init_common(MemoryRegion *mr, MemoryRegionKind kind, Object *owner, const char *name, uint64_t size)
{
    check_not_frozen("memory_region_init", name);
    memset(mr, 0, sizeof *mr);
    mr->kind = kind;
    mr->owner = owner;
    mr->name = name;
    mr->size = size;
}

void memory_region_init(MemoryRegion *mr, Object *owner, const char *name, uint64_t size)
{
    init_common(mr, MR_CONTAINER, owner, name, size);
}

void memory_region_init_io(MemoryRegion *mr, Object *owner, const MemoryRegionOps *ops, void *opaque,
                           const char *name, uint64_t size)
{
    init_common(mr, MR_IO, owner, name, size);
    mr->ops = ops;
    mr->opaque = opaque;
}

void memory_region_init_ram_nomigrate(MemoryRegion *mr, Object *owner, const char *name, uint64_t size,
                                      Error **errp)
{
    (void)errp;
    init_common(mr, MR_RAM, owner, name, size);
    mr->ram = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mr->ram) xbrt_refuse(XBRT_REFUSE_FEATURE, "no memory for RAM region \"%s\" (%llu bytes)", name, size);
    mr->ram_owned = true;
}

void memory_region_init_ram(MemoryRegion *mr, Object *owner, const char *name, uint64_t size, Error **errp)
{
    memory_region_init_ram_nomigrate(mr, owner, name, size, errp);
}

void memory_region_init_alias(MemoryRegion *mr, Object *owner, const char *name, MemoryRegion *orig, hwaddr offset,
                              uint64_t size)
{
    init_common(mr, MR_ALIAS, owner, name, size);
    mr->alias = orig;
    mr->alias_offset = offset;
}

void memory_region_add_subregion(MemoryRegion *mr, hwaddr offset, MemoryRegion *subregion)
{
    check_not_frozen("memory_region_add_subregion", mr->name);
    if (mr->nsub == XBRT_MAX_SUBREGIONS)
        xbrt_refuse(XBRT_REFUSE_FEATURE, "\"%s\" has more than %d subregions", mr->name, XBRT_MAX_SUBREGIONS);
    subregion->container = mr;
    subregion->addr = offset;
    mr->sub[mr->nsub++] = subregion;
}

void memory_region_add_subregion_overlap(MemoryRegion *mr, hwaddr offset, MemoryRegion *subregion, int priority)
{
    (void)priority;
    memory_region_add_subregion(mr, offset, subregion);
}

void memory_region_del_subregion(MemoryRegion *mr, MemoryRegion *subregion)
{
    check_not_frozen("memory_region_del_subregion", mr->name);
    for (unsigned i = 0; i < mr->nsub; i++) {
        if (mr->sub[i] != subregion) continue;
        memmove(&mr->sub[i], &mr->sub[i + 1], (mr->nsub - i - 1) * sizeof mr->sub[0]);
        mr->nsub--;
        subregion->container = NULL;
        return;
    }
}

void memory_region_destroy(MemoryRegion *mr)
{
    check_not_frozen("memory_region_destroy", mr->name);
    if (mr->ram_owned) VirtualFree(mr->ram, 0, MEM_RELEASE);
    memset(mr, 0, sizeof *mr);
}

uint64_t memory_region_size(MemoryRegion *mr) { return mr->size; }
Object *memory_region_owner(MemoryRegion *mr) { return mr->owner; }

void *memory_region_get_ram_ptr(MemoryRegion *mr)
{
    hwaddr offset = 0;
    while (mr->kind == MR_ALIAS) {
        offset += mr->alias_offset;
        mr = mr->alias;
    }
    if (mr->kind != MR_RAM) xbrt_refuse(XBRT_REFUSE_FEATURE, "\"%s\" is not RAM", mr->name);
    return mr->ram + offset;
}

void memory_region_set_log(MemoryRegion *mr, bool log, unsigned client)
{
    if (log) mr->dirty_log_mask |= 1u << client;
    else mr->dirty_log_mask &= ~(1u << client);
}

void memory_region_set_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size)
{
    (void)mr;
    (void)addr;
    (void)size;
}

/* Guest code runs natively, so no store is ever seen: every page answers dirty, which over-invalidates and never serves
 * stale data. */
bool memory_region_test_and_clear_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client)
{
    (void)mr;
    (void)addr;
    (void)size;
    (void)client;
    return true;
}

void memory_region_set_coalescing(MemoryRegion *mr) { (void)mr; }
void memory_region_set_flush_coalesced(MemoryRegion *mr) { (void)mr; }
void qemu_flush_coalesced_mmio_buffer(void) {}

void xbrt_memory_add_bar(PCIDevice *dev, int index, MemoryRegion *root)
{
    check_not_frozen("pci_register_bar", root->name);
    if (g_bar_count == MAX_BARS) xbrt_refuse(XBRT_REFUSE_FEATURE, "more than %d BARs", MAX_BARS);
    g_bars[g_bar_count++] = (Bar){.root = root, .dev = dev, .index = index};
}

static void emit(Bar *bar, uint32_t start, uint32_t end, MemoryRegion *mr, uint32_t mr_offset)
{
    if (start == end) return;
    if (bar->count == MAX_RANGES)
        xbrt_refuse(XBRT_REFUSE_FEATURE, "%s BAR%d flattens to more than %d ranges", bar->device, bar->index,
                    MAX_RANGES);
    bar->ranges[bar->count++] = (FlatRange){start, end, mr, mr_offset};
}

static int by_addr(const void *a, const void *b)
{
    hwaddr x = (*(MemoryRegion *const *)a)->addr, y = (*(MemoryRegion *const *)b)->addr;
    return x < y ? -1 : x > y;
}

static bool default_access(const MemoryRegionAccess *a)
{
    return !a->min_access_size && !a->max_access_size && !a->unaligned;
}

/* QEMU's rule: a region's subregions cover it, and an io or RAM region with subregions serves the gaps itself. */
static void flatten(Bar *bar, MemoryRegion *mr, uint32_t bar_base, uint32_t mr_offset, uint32_t size)
{
    if (mr->kind == MR_ALIAS) {
        if (mr->alias_offset + mr_offset + size > mr->alias->size)
            xbrt_refuse(XBRT_REFUSE_FEATURE, "alias \"%s\" reaches past \"%s\"", mr->name, mr->alias->name);
        flatten(bar, mr->alias, bar_base, (uint32_t)mr->alias_offset + mr_offset, size);
        return;
    }
    if (mr->kind == MR_IO && (!default_access(&mr->ops->valid) || !default_access(&mr->ops->impl) ||
                              mr->ops->endianness == DEVICE_BIG_ENDIAN))
        xbrt_refuse(XBRT_REFUSE_FEATURE, "\"%s\" asks for access-size adaptation, which the shim does not do",
                    mr->name);
    MemoryRegion *subs[XBRT_MAX_SUBREGIONS];
    memcpy(subs, mr->sub, mr->nsub * sizeof subs[0]);
    qsort(subs, mr->nsub, sizeof subs[0], by_addr);
    bool serves_gaps = mr->kind != MR_CONTAINER;
    uint32_t cursor = mr_offset, end = mr_offset + size, previous_end = 0;
    for (unsigned i = 0; i < mr->nsub; i++) {
        uint32_t s = (uint32_t)subs[i]->addr, e = s + (uint32_t)subs[i]->size;
        if (s < previous_end)
            xbrt_refuse(XBRT_REFUSE_FEATURE, "subregion \"%s\" of \"%s\" overlaps another", subs[i]->name, mr->name);
        previous_end = e;
        if (e <= mr_offset || s >= end) continue;
        uint32_t from = MAX(s, mr_offset), to = MIN(e, end);
        if (serves_gaps) emit(bar, bar_base + (cursor - mr_offset), bar_base + (from - mr_offset), mr, cursor);
        flatten(bar, subs[i], bar_base + (from - mr_offset), from - s, to - from);
        cursor = to;
    }
    if (serves_gaps) emit(bar, bar_base + (cursor - mr_offset), bar_base + size, mr, cursor);
}

unsigned xbrt_memory_freeze(const XbrtDeviceSpec *devices, XbrtBar *bars, unsigned max_bars)
{
    if (g_bar_count > max_bars) xbrt_refuse(XBRT_REFUSE_FEATURE, "%u BARs, room for %u", g_bar_count, max_bars);
    for (unsigned i = 0; i < g_bar_count; i++) {
        Bar *b = &g_bars[i];
        b->device = devices[b->dev->irq_line].device;
        if (b->root->size > UINT32_MAX) xbrt_refuse(XBRT_REFUSE_FEATURE, "%s BAR%d is over 4 GB", b->device, b->index);
        flatten(b, b->root, 0, 0, (uint32_t)b->root->size);
        const FlatRange *r = &b->ranges[0];
        bool ram_alias = b->count == 1 && r->mr == &xbrt_board_ram && r->mr_offset == 0 && r->end == xbrt_board_ram.size;
        bars[i] = (XbrtBar){b->device, (uint8_t)b->index, (uint8_t)b->dev->irq_line, ram_alias, (uint32_t)b->root->size};
    }
    g_frozen = true;
    return g_bar_count;
}

static const FlatRange *lookup(const Bar *bar, uint32_t offset)
{
    unsigned lo = 0, hi = bar->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (offset < bar->ranges[mid].start) hi = mid;
        else if (offset >= bar->ranges[mid].end) lo = mid + 1;
        else return &bar->ranges[mid];
    }
    return NULL;
}

static const FlatRange *checked(unsigned bar, uint32_t offset, unsigned size, const char *verb)
{
    const Bar *b = &g_bars[bar];
    if (offset & (size - 1))
        xbrt_refuse(XBRT_REFUSE_DEVICE_REGISTER, "%s register 0x%X %s of %u bytes: unaligned", b->device, offset, verb,
                    size);
    const FlatRange *r = lookup(b, offset);
    if (!r || offset + size > r->end)
        xbrt_refuse(XBRT_REFUSE_DEVICE_REGISTER, "%s register 0x%X %s of %u bytes: no region (hole)", b->device,
                    offset, verb, size);
    return r;
}

uint32_t xbrt_xemu_read(unsigned bar, uint32_t offset, unsigned size)
{
    const FlatRange *r = checked(bar, offset, size, "read");
    uint32_t at = r->mr_offset + (offset - r->start);
    uint32_t v = 0;
    if (r->mr->kind == MR_RAM) {
        memcpy(&v, r->mr->ram + at, size);
        return v;
    }
    bql_lock();
    v = (uint32_t)r->mr->ops->read(r->mr->opaque, at, size);
    bql_unlock();
    return size == 4 ? v : v & ((1u << (size * 8)) - 1);
}

void xbrt_xemu_write(unsigned bar, uint32_t offset, unsigned size, uint32_t value)
{
    const FlatRange *r = checked(bar, offset, size, "write");
    uint32_t at = r->mr_offset + (offset - r->start);
    if (r->mr->kind == MR_RAM) {
        memcpy(r->mr->ram + at, &value, size);
        return;
    }
    bql_lock();
    r->mr->ops->write(r->mr->opaque, at, value, size);
    bql_unlock();
}

static uint8_t *phys(hwaddr addr, unsigned size, const char *verb)
{
    if (addr + size > xbrt_board_ram.size || addr + size < addr)
        xbrt_refuse(XBRT_REFUSE_DEVICE_REGISTER, "a device %s physical 0x%llX: outside RAM", verb,
                    (unsigned long long)addr);
    return xbrt_board_ram.ram + addr;
}

uint32_t ldub_phys(AddressSpace *as, hwaddr addr) { (void)as; return *phys(addr, 1, "read"); }
uint32_t lduw_le_phys(AddressSpace *as, hwaddr addr) { (void)as; return (uint32_t)lduw_le_p(phys(addr, 2, "read")); }
uint32_t ldl_le_phys(AddressSpace *as, hwaddr addr) { (void)as; return (uint32_t)ldl_le_p(phys(addr, 4, "read")); }
uint64_t ldq_le_phys(AddressSpace *as, hwaddr addr) { (void)as; return ldq_le_p(phys(addr, 8, "read")); }
void stb_phys(AddressSpace *as, hwaddr addr, uint32_t val) { (void)as; *phys(addr, 1, "wrote") = (uint8_t)val; }
void stw_le_phys(AddressSpace *as, hwaddr addr, uint32_t val) { (void)as; stw_le_p(phys(addr, 2, "wrote"), (uint16_t)val); }
void stl_le_phys(AddressSpace *as, hwaddr addr, uint32_t val) { (void)as; stl_le_p(phys(addr, 4, "wrote"), val); }
void stq_le_phys(AddressSpace *as, hwaddr addr, uint64_t val) { (void)as; stq_le_p(phys(addr, 8, "wrote"), val); }
