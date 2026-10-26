#include <windows.h>
#include <setjmp.h>

#include "../../host/xemu/shim/src/shim.h"
#include "hw/xbox/mcpx/apu/apu_regs.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "renderers/probe.h"

void mcpx_apu_init(PCIBus *bus, int devfn, MemoryRegion *ram);
void nv2a_init(PCIBus *bus, int devfn, MemoryRegion *ram);

static DWORD g_test_thread;
static jmp_buf g_refused;
static bool g_expect_refusal;
static XbrtRefusal g_refusal_kind;
static char g_refusal[512];

static void test_refuse(XbrtRefusal kind, const char *what)
{
    g_refusal_kind = kind;
    snprintf(g_refusal, sizeof g_refusal, "%s", what);
    if (g_expect_refusal && GetCurrentThreadId() == g_test_thread) longjmp(g_refused, 1);
    printf("xemu tests: FAIL unexpected refusal on thread %lu: %s\n", GetCurrentThreadId(), what);
    fflush(stdout);
    ExitProcess(1);
}

static void test_log(const char *line) { printf("  log: %s\n", line); }

/* The NV2A recomputes its line on every vblank, so calls that keep the level are not recorded; any call without the
 * BQL is. */
typedef struct IrqCall {
    unsigned line;
    int level;
} IrqCall;

static IrqCall g_irq_calls[4096];
static volatile LONG g_irq_count, g_irq_without_bql;
static volatile LONG g_level[8];
static HANDLE g_irq_wake;

static void test_irq_level(unsigned line, int level)
{
    if (!bql_locked()) InterlockedExchange(&g_irq_without_bql, 1);
    if (InterlockedExchange(&g_level[line], level) == level) return;
    LONG i = InterlockedIncrement(&g_irq_count) - 1;
    if (i < (LONG)ARRAY_SIZE(g_irq_calls)) g_irq_calls[i] = (IrqCall){line, level};
    if (level) SetEvent(g_irq_wake);
}

static bool wait_level(unsigned line, LONG level, DWORD ms)
{
    for (ULONGLONG start = GetTickCount64(); g_level[line] != level;) {
        if (GetTickCount64() - start > ms) return false;
        Sleep(1);
    }
    return true;
}

static bool refuses(void (*fn)(void *), void *arg)
{
    g_refusal[0] = 0;
    g_expect_refusal = true;
    if (setjmp(g_refused)) {
        g_expect_refusal = false;
        return true;
    }
    fn(arg);
    g_expect_refusal = false;
    return false;
}

typedef struct TestDev {
    PCIDevice parent_obj;
    MemoryRegion bar0, a, b, ram, bar1, c;
} TestDev;

typedef struct Access {
    unsigned region;
    uint32_t addr;
    unsigned size;
    uint32_t value;
    bool write;
} Access;

static Access g_last;
static unsigned g_accesses;

static uint64_t test_read(void *opaque, hwaddr addr, unsigned size)
{
    unsigned region = (unsigned)(uintptr_t)opaque;
    uint32_t v = 0x9A000000u | region << 16 | (uint32_t)addr;
    g_last = (Access){region, (uint32_t)addr, size, v, false};
    g_accesses++;
    return v;
}

static void test_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    g_last = (Access){(unsigned)(uintptr_t)opaque, (uint32_t)addr, size, (uint32_t)value, true};
    g_accesses++;
}

static const MemoryRegionOps test_ops = {.read = test_read, .write = test_write};

enum { REGION_A = 1, REGION_B, REGION_C, REGION_BAR1 };

static TestDev *g_dev;

static void test_dev_realize(PCIDevice *dev, Error **errp)
{
    TestDev *d = (TestDev *)dev;
    memory_region_init(&d->bar0, OBJECT(d), "test-bar0", 0x10000);
    memory_region_init_io(&d->a, OBJECT(d), &test_ops, (void *)(uintptr_t)REGION_A, "test-a", 0x1000);
    memory_region_add_subregion(&d->bar0, 0x0000, &d->a);
    memory_region_init_io(&d->b, OBJECT(d), &test_ops, (void *)(uintptr_t)REGION_B, "test-b", 0x1000);
    memory_region_add_subregion(&d->bar0, 0x2000, &d->b);
    memory_region_init_ram(&d->ram, OBJECT(d), "test-ram", 0x1000, errp);
    memory_region_add_subregion(&d->bar0, 0x8000, &d->ram);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->bar0);
    memory_region_init_io(&d->bar1, OBJECT(d), &test_ops, (void *)(uintptr_t)REGION_BAR1, "test-bar1", 0x4000);
    memory_region_init_io(&d->c, OBJECT(d), &test_ops, (void *)(uintptr_t)REGION_C, "test-c", 0x1000);
    memory_region_add_subregion(&d->bar1, 0x1000, &d->c);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->bar1);
    g_dev = d;
}

static void test_dev_class_init(ObjectClass *klass, const void *data)
{
    (void)data;
    PCI_DEVICE_CLASS(klass)->realize = test_dev_realize;
}

static const TypeInfo test_dev_info = {
    .name = "xbrt-test-dev",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(TestDev),
    .class_init = test_dev_class_init,
};

static void test_dev_register(void) { type_register_static(&test_dev_info); }
type_init(test_dev_register)

static void test_dev_init(PCIBus *bus, int devfn, MemoryRegion *ram)
{
    (void)ram;
    pci_create_simple(bus, devfn, "xbrt-test-dev");
}

static const XbrtDeviceSpec kSpecs[] = {
    {"TEST", test_dev_init, 0},
    {"APU", mcpx_apu_init, PCI_DEVFN(5, 0)},
    {"NV2A", nv2a_init, PCI_DEVFN(0, 0)},
};
enum { BAR_TEST0, BAR_TEST1, BAR_APU, BAR_NV2A, BAR_NV2A_RAM, BAR_COUNT };
enum { LINE_TEST, LINE_APU, LINE_NV2A };
static XbrtHost g_host;

typedef struct Result {
    bool ok;
    char detail[256];
} Result;

#define RESULT(ok, ...)                                                                                               \
    do {                                                                                                               \
        Result r_ = {(ok)};                                                                                            \
        snprintf(r_.detail, sizeof r_.detail, __VA_ARGS__);                                                            \
        return r_;                                                                                                     \
    } while (0)

/* Defect: a BAR offset not rebased to its subregion, or an io region with subregions not serving its own gaps. */
static Result flatten_and_dispatch(void)
{
    static const struct {
        unsigned bar;
        uint32_t offset, region, addr;
    } cases[] = {
        {BAR_TEST0, 0x0010, REGION_A, 0x0010}, {BAR_TEST0, 0x2010, REGION_B, 0x0010},
        {BAR_TEST1, 0x0010, REGION_BAR1, 0x0010}, {BAR_TEST1, 0x1010, REGION_C, 0x0010},
        {BAR_TEST1, 0x2010, REGION_BAR1, 0x2010}, {BAR_TEST1, 0x3FFC, REGION_BAR1, 0x3FFC},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        uint32_t want = 0x9A000000u | cases[i].region << 16 | cases[i].addr;
        uint32_t got = xbrt_xemu_read(cases[i].bar, cases[i].offset, 4);
        if (got != want || g_last.region != cases[i].region || g_last.addr != cases[i].addr)
            RESULT(false, "BAR %u +0x%X read 0x%08X, want 0x%08X (region %u addr 0x%X)", cases[i].bar,
                   cases[i].offset, got, want, g_last.region, g_last.addr);
    }
    xbrt_xemu_write(BAR_TEST0, 0x2020, 4, 0xDEADBEEF);
    if (!g_last.write || g_last.region != REGION_B || g_last.addr != 0x20 || g_last.value != 0xDEADBEEF)
        RESULT(false, "write reached region %u addr 0x%X value 0x%08X", g_last.region, g_last.addr, g_last.value);
    RESULT(true, "6 reads and a write reached their regions at region-relative offsets");
}

/* Defect: the op's 64-bit result leaking into a narrower guest register. */
static Result read_masked_to_size(void)
{
    uint32_t b = xbrt_xemu_read(BAR_TEST0, 0x0011, 1);
    unsigned b_size = g_last.size;
    uint32_t w = xbrt_xemu_read(BAR_TEST0, 0x0012, 2);
    unsigned w_size = g_last.size;
    bool ok = b == 0x11 && w == 0x0012 && b_size == 1 && w_size == 2;
    RESULT(ok, "byte 0x%X (op saw size %u), word 0x%X (op saw size %u)", b, b_size, w, w_size);
}

static void read_hole(void *arg) { (void)arg; xbrt_xemu_read(BAR_TEST0, 0x1000, 4); }
static void read_unaligned(void *arg) { (void)arg; xbrt_xemu_read(BAR_TEST0, 0x0002, 4); }

/* Defect: a hole that reads as 0 and lets the guest run on. */
static Result hole_refuses(void)
{
    unsigned before = g_accesses;
    bool refused = refuses(read_hole, NULL);
    bool named = strstr(g_refusal, "hole") && strstr(g_refusal, "0x1000");
    RESULT(refused && named && g_refusal_kind == XBRT_REFUSE_DEVICE_REGISTER && g_accesses == before, "%s",
           g_refusal[0] ? g_refusal : "no refusal");
}

/* Defect: a torn or silently split unaligned access. */
static Result unaligned_refuses(void)
{
    unsigned before = g_accesses;
    bool refused = refuses(read_unaligned, NULL);
    RESULT(refused && strstr(g_refusal, "unaligned") && g_accesses == before, "%s",
           g_refusal[0] ? g_refusal : "no refusal");
}

/* Defect: a RAM block inside a BAR dispatched to an op, or written to a copy the device never reads. */
static Result ram_subregion_is_memory(void)
{
    unsigned before = g_accesses;
    xbrt_xemu_write(BAR_TEST0, 0x8004, 4, 0xCAFEF00D);
    uint32_t back = xbrt_xemu_read(BAR_TEST0, 0x8004, 4);
    uint32_t backing = ldl_le_p((uint8_t *)memory_region_get_ram_ptr(&g_dev->ram) + 4);
    RESULT(back == 0xCAFEF00D && backing == 0xCAFEF00D && g_accesses == before,
           "read back 0x%08X, backing 0x%08X, ops called %u times", back, backing, g_accesses - before);
}

static void add_after_freeze(void *arg)
{
    (void)arg;
    static MemoryRegion late;
    memory_region_init_io(&late, NULL, &test_ops, (void *)(uintptr_t)REGION_A, "late", 0x1000);
}

/* Defect: a map change after the flat view was built, served from the stale view. */
static Result map_change_after_freeze_refuses(void)
{
    bool refused = refuses(add_after_freeze, NULL);
    RESULT(refused && strstr(g_refusal, "frozen"), "%s", g_refusal[0] ? g_refusal : "no refusal");
}

static void lock_twice(void *arg)
{
    (void)arg;
    bql_lock();
    bql_lock();
}

/* Defect: a recursive BQL acquire that deadlocks silently. */
static Result bql_recursive_refuses(void)
{
    bool refused = refuses(lock_twice, NULL);
    bool released = !bql_locked();
    RESULT(refused && strstr(g_refusal, "twice") && released, "%s; BQL %s after the refusal",
           g_refusal[0] ? g_refusal : "no refusal", released ? "released" : "still held");
}

static int g_fired[8], g_fired_count;
static bool g_fired_with_bql[8];

static void timer_cb(void *opaque)
{
    if (g_fired_count < 8) {
        g_fired_with_bql[g_fired_count] = bql_locked();
        g_fired[g_fired_count++] = (int)(intptr_t)opaque;
    }
}

/* Defect: timers out of expiry order, a callback racing MMIO without the BQL, or a deleted timer firing. */
static Result timers_fire_in_order_with_bql(void)
{
    static QEMUTimer t1, t2, t3;
    timer_init_ns(&t1, QEMU_CLOCK_VIRTUAL, timer_cb, (void *)1);
    timer_init_ns(&t2, QEMU_CLOCK_VIRTUAL, timer_cb, (void *)2);
    timer_init_ns(&t3, QEMU_CLOCK_VIRTUAL, timer_cb, (void *)3);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bql_lock();
    timer_mod(&t2, now + 20 * SCALE_MS);
    timer_mod(&t3, now + 15 * SCALE_MS);
    timer_mod(&t1, now + 10 * SCALE_MS);
    timer_del(&t3);
    bql_unlock();
    Sleep(100);
    bool ok = g_fired_count == 2 && g_fired[0] == 1 && g_fired[1] == 2 && g_fired_with_bql[0] && g_fired_with_bql[1];
    RESULT(ok, "fired %d: %d (bql %d) %d (bql %d)", g_fired_count, g_fired[0], g_fired_with_bql[0], g_fired[1],
           g_fired_with_bql[1]);
}

static void gp_word_read(void *arg)
{
    (void)arg;
    xbrt_xemu_read(BAR_APU, 0x30000, 2);
}

/* Defect: an upstream assert that aborts (a message box in a windows-subsystem app) instead of naming itself. Upstream's
 * GP register read asserts a 4-byte access. */
static Result assert_is_a_refusal(void)
{
    bool refused = refuses(gp_word_read, NULL);
    bool named = strstr(g_refusal, "size == 4") && strstr(g_refusal, "gp_ep.c");
    RESULT(refused && named && g_refusal_kind == XBRT_REFUSE_FEATURE && !bql_locked(), "%s",
           g_refusal[0] ? g_refusal : "no refusal");
}

/* Defect: an irq_level call made without the BQL (two writers of one line), or a line that never rises or never drops
 * once acknowledged, or an ISR that cannot read device registers once the line is up. */
static Result irq_level_with_bql_and_isr_reenters(void)
{
    LONG first = g_irq_count;
    ResetEvent(g_irq_wake);
    xbrt_xemu_write(BAR_APU, NV_PAPU_IEN, 4, NV_PAPU_ISTS_GINTSTS | NV_PAPU_ISTS_FETINTSTS);
    xbrt_xemu_write(BAR_APU, NV_PAPU_FECTL, 4, NV_PAPU_FECTL_FEMETHMODE_TRAPPED);
    DWORD wait = WaitForSingleObject(g_irq_wake, 1000);
    uint32_t ists = xbrt_xemu_read(BAR_APU, NV_PAPU_ISTS, 4);
    xbrt_xemu_write(BAR_APU, NV_PAPU_FECTL, 4, 0);
    xbrt_xemu_write(BAR_APU, NV_PAPU_ISTS, 4, NV_PAPU_ISTS_FETINTSTS | NV_PAPU_ISTS_GINTSTS);
    Sleep(20);
    LONG last = g_irq_count;
    bool rose = false, dropped_last = false, all_with_bql = !g_irq_without_bql;
    for (LONG i = first; i < last && i < (LONG)ARRAY_SIZE(g_irq_calls); i++) {
        if (g_irq_calls[i].line != LINE_APU) continue;
        rose |= g_irq_calls[i].level == 1;
        dropped_last = g_irq_calls[i].level == 0;
    }
    bool ok = wait == WAIT_OBJECT_0 && ists == 0x11 && rose && dropped_last && all_with_bql;
    RESULT(ok, "wait %lu, ISTS 0x%08X, %ld level changes, rose %d, last level 0 %d, all with the BQL %d", wait,
           ists, last - first, rose, dropped_last, all_with_bql);
}

/* A 128-bit reference: the product by parts and a restoring division, so neither side calls __udivti3. */
static uint64_t reference_muldiv(uint64_t a, uint32_t b, uint32_t c, bool round_up)
{
    uint64_t lo = (a & 0xFFFFFFFFu) * b, mid = (a >> 32) * b;
    uint64_t hi = mid >> 32;
    uint64_t low = lo + (mid << 32);
    if (low < lo) hi++;
    if (round_up) {
        uint64_t before = low;
        low += c - 1;
        if (low < before) hi++;
    }
    uint64_t q = 0, r = 0;
    for (int bit = 127; bit >= 0; bit--) {
        r = r << 1 | (bit >= 64 ? hi >> (bit - 64) & 1 : low >> bit & 1);
        q <<= 1;
        if (r >= c) {
            r -= c;
            q |= 1;
        }
    }
    return q;
}

/* Defect: a muldiv64 that loses the high part of a wide product (PTIMER's ns scaling), or one that needs __udivti3 to
 * link. Both sides keep the quotient modulo 2^64, as QEMU's does. */
static Result muldiv64_wide(void)
{
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (unsigned i = 0; i < 2000000; i++) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        uint64_t a = x;
        uint32_t b = (uint32_t)(x >> 17) | 1, c = (uint32_t)(x >> 29) | 1;
        if (i % 3 == 0) c |= 0x80000000u;
        if (i % 3 == 1) a >>= x & 31;
        uint64_t want = reference_muldiv(a, b, c, false), want_up = reference_muldiv(a, b, c, true);
        if (muldiv64(a, b, c) != want || muldiv64_round_up(a, b, c) != want_up)
            RESULT(false, "muldiv64(0x%llX, %u, %u) = 0x%llX (round up 0x%llX), want 0x%llX (0x%llX)",
                   (unsigned long long)a, b, c, (unsigned long long)muldiv64(a, b, c),
                   (unsigned long long)muldiv64_round_up(a, b, c), (unsigned long long)want,
                   (unsigned long long)want_up);
    }
    uint64_t ns = muldiv64(0x7FFFFFFFFFFFull, 1000000000u, 733333333u);
    RESULT(ns == reference_muldiv(0x7FFFFFFFFFFFull, 1000000000u, 733333333u, false),
           "2000000 random cases agree with a 128-bit restoring division");
}

/* Defect: the APU frame thread never un-paused (no vm start), or millisecond waits at the 15.6 ms default timer
 * resolution, which runs XGSCNT at about a third of 48 kHz. */
static Result apu_xgscnt_off_then_48k(void)
{
    uint32_t off0 = xbrt_xemu_read(BAR_APU, NV_PAPU_XGSCNT, 4);
    Sleep(30);
    uint32_t off1 = xbrt_xemu_read(BAR_APU, NV_PAPU_XGSCNT, 4);
    /* Reset leaves each voice list's top at voice 0; a driver empties them (0xFFFF) before it starts the frame clock,
     * or the first frame idles voice 0 and upstream asserts. */
    xbrt_xemu_write(BAR_APU, NV_PAPU_TVL2D, 4, 0xFFFF);
    xbrt_xemu_write(BAR_APU, NV_PAPU_TVL3D, 4, 0xFFFF);
    xbrt_xemu_write(BAR_APU, NV_PAPU_TVLMP, 4, 0xFFFF);
    xbrt_xemu_write(BAR_APU, NV_PAPU_SECTL, 4, 1 << 3);
    Sleep(30);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    uint32_t c0 = xbrt_xemu_read(BAR_APU, NV_PAPU_XGSCNT, 4);
    QueryPerformanceCounter(&t0);
    Sleep(1000);
    uint32_t c1 = xbrt_xemu_read(BAR_APU, NV_PAPU_XGSCNT, 4);
    QueryPerformanceCounter(&t1);
    xbrt_xemu_write(BAR_APU, NV_PAPU_SECTL, 4, 0);
    double seconds = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    double rate = (c1 - c0) / seconds;
    bool ok = off0 == off1 && rate > 40000 && rate < 56000;
    RESULT(ok, "off %u then %u; on: %.0f samples per second", off0, off1, rate);
}

enum {
    NV2A_PMC = 0x000000,
    NV2A_PFIFO = 0x002000,
    NV2A_PTIMER = 0x009000,
    NV2A_PGRAPH = 0x400000,
    NV2A_PCRTC = 0x600000,
    NV2A_RAMIN = 0x700000,
    NV2A_USER = 0x800000,
};

static uint32_t nv2a_r(uint32_t offset) { return xbrt_xemu_read(BAR_NV2A, offset, 4); }
static void nv2a_w(uint32_t offset, uint32_t value) { xbrt_xemu_write(BAR_NV2A, offset, 4, value); }

static XbrtBar g_bars[8];

static Result nv2a_boot0_and_ram_bar(void)
{
    uint32_t boot0 = nv2a_r(NV2A_PMC + NV_PMC_BOOT_0);
    const XbrtBar *ram = &g_bars[BAR_NV2A_RAM];
    bool ok = boot0 == 0x02A000A3 && !g_bars[BAR_NV2A].ram_alias && g_bars[BAR_NV2A].size == 0x1000000 &&
              ram->ram_alias && ram->index == 1 && ram->size == g_host.ram_bytes;
    RESULT(ok, "boot0 0x%08X, BAR1 ram alias %u index %u size 0x%X", boot0, ram->ram_alias, ram->index, ram->size);
}

enum { RAMHT = 0x10000, DMA_PUSH_BUFFER = 0x20000, DMA_SEMAPHORE = 0x20010, KELVIN = 0x20020 };
enum { H_KELVIN = 0x11, H_SEMAPHORE = 0x12 };
enum { PUSH_BUFFER = 0x100000, SEMAPHORE = 0x101000 };

static uint32_t ramht_hash(uint32_t handle)
{
    uint32_t hash = 0;
    for (; handle; handle >>= 11) hash ^= handle & 0x7FF;
    return hash;
}

static void dma_object(uint32_t instance, uint32_t physical, uint32_t limit)
{
    nv2a_w(NV2A_RAMIN + instance, NV_DMA_IN_MEMORY_CLASS | NV_DMA_TARGET_PCI | (physical & 0xFFF) << 20);
    nv2a_w(NV2A_RAMIN + instance + 4, limit);
    nv2a_w(NV2A_RAMIN + instance + 8, physical & NV_DMA_ADDRESS);
}

static void ramht_entry(uint32_t handle, uint32_t instance)
{
    uint32_t at = NV2A_RAMIN + RAMHT + ramht_hash(handle) * 8;
    nv2a_w(at, handle);
    nv2a_w(at + 4, instance >> 4 | NV_RAMHT_ENGINE_GRAPHICS | NV_RAMHT_STATUS);
}

static uint32_t method(uint32_t subchannel, uint32_t m, uint32_t count) { return count << 18 | subchannel << 13 | m; }

static Result nv2a_push_buffer_context_switch_semaphore(void)
{
    XbrtCounters before;
    xbrt_xemu_counters(&before);
    uint32_t *pb = (uint32_t *)(g_host.ram + PUSH_BUFFER);
    uint32_t *semaphore = (uint32_t *)(g_host.ram + SEMAPHORE);
    *semaphore = 0;
    dma_object(DMA_PUSH_BUFFER, PUSH_BUFFER, 0xFFF);
    dma_object(DMA_SEMAPHORE, SEMAPHORE, 0xFFF);
    nv2a_w(NV2A_RAMIN + KELVIN, NV_KELVIN_PRIMITIVE);
    ramht_entry(H_KELVIN, KELVIN);
    ramht_entry(H_SEMAPHORE, DMA_SEMAPHORE);
    unsigned n = 0;
    pb[n++] = method(0, NV097_SET_OBJECT, 1), pb[n++] = H_KELVIN;
    pb[n++] = method(0, NV097_SET_CONTEXT_DMA_SEMAPHORE, 1), pb[n++] = H_SEMAPHORE;
    pb[n++] = method(0, NV097_SET_SEMAPHORE_OFFSET, 1), pb[n++] = 0;
    pb[n++] = method(0, NV097_BACK_END_WRITE_SEMAPHORE_RELEASE, 1), pb[n++] = 0x00C0FFEE;
    while (n < 16) pb[n++] = method(0, NV097_NO_OPERATION, 1), pb[n++] = 0;

    nv2a_w(NV2A_PFIFO + NV_PFIFO_RAMHT, (RAMHT >> 12) << 4 | NV_PFIFO_RAMHT_SIZE_4K << 16);
    nv2a_w(NV2A_PFIFO + NV_PFIFO_MODE, 1);
    nv2a_w(NV2A_PFIFO + NV_PFIFO_CACHE1_PUSH1, NV_PFIFO_CACHE1_PUSH1_MODE);
    nv2a_w(NV2A_PFIFO + NV_PFIFO_CACHE1_DMA_INSTANCE, DMA_PUSH_BUFFER >> 4);
    nv2a_w(NV2A_PFIFO + NV_PFIFO_CACHE1_DMA_PUSH, NV_PFIFO_CACHE1_DMA_PUSH_ACCESS);
    nv2a_w(NV2A_PFIFO + NV_PFIFO_CACHE1_PUSH0, NV_PFIFO_CACHE1_PUSH0_ACCESS);
    nv2a_w(NV2A_PFIFO + NV_PFIFO_CACHE1_PULL0, NV_PFIFO_CACHE1_PULL0_ACCESS);
    nv2a_w(NV2A_PGRAPH + NV_PGRAPH_INTR_EN, NV_PGRAPH_INTR_EN_CONTEXT_SWITCH);
    nv2a_w(NV2A_PGRAPH + NV_PGRAPH_FIFO, NV_PGRAPH_FIFO_ACCESS);
    nv2a_w(NV2A_PMC + NV_PMC_INTR_EN_0, NV_PMC_INTR_EN_0_HARDWARE);
    nv2a_w(NV2A_USER + NV_USER_DMA_PUT, n * 4);

    bool rose = wait_level(LINE_NV2A, 1, 1000);
    uint32_t pmc = nv2a_r(NV2A_PMC + NV_PMC_INTR_0), intr = nv2a_r(NV2A_PGRAPH + NV_PGRAPH_INTR);
    nv2a_w(NV2A_PGRAPH + NV_PGRAPH_CTX_USER, 0);
    nv2a_w(NV2A_PGRAPH + NV_PGRAPH_CTX_CONTROL, NV_PGRAPH_CTX_CONTROL_CHID);
    nv2a_w(NV2A_PGRAPH + NV_PGRAPH_INTR, intr);
    nv2a_w(NV2A_PMC + NV_PMC_INTR_0, pmc);
    bool dropped = wait_level(LINE_NV2A, 0, 1000);
    uint32_t get = 0;
    for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 1000; Sleep(1))
        if ((get = nv2a_r(NV2A_USER + NV_USER_DMA_GET)) == n * 4) break;
    XbrtCounters after;
    xbrt_xemu_counters(&after);
    bool ok = rose && pmc == NV_PMC_INTR_0_PGRAPH && intr == NV_PGRAPH_INTR_CONTEXT_SWITCH && dropped && get == n * 4 &&
              *semaphore == 0x00C0FFEE && after.nv2a_methods - before.nv2a_methods == n / 2;
    RESULT(ok, "line rose %d (PMC 0x%08X, PGRAPH 0x%08X), dropped %d, GET 0x%X of 0x%X, semaphore 0x%08X, methods %llu",
           rose, pmc, intr, dropped, get, n * 4, *semaphore,
           (unsigned long long)(after.nv2a_methods - before.nv2a_methods));
}

static Result probe_counts_one_draw(void)
{
    XbrtProbeCounts p0 = xbrt_probe_counts;
    XbrtCounters c0;
    xbrt_xemu_counters(&c0);
    uint32_t *pb = (uint32_t *)(g_host.ram + PUSH_BUFFER);
    uint32_t start = nv2a_r(NV2A_USER + NV_USER_DMA_GET), n = start / 4;
    pb[n++] = method(0, NV097_SET_BEGIN_END, 1), pb[n++] = NV097_SET_BEGIN_END_OP_TRIANGLES;
    pb[n++] = 0x40000000u | method(0, NV097_INLINE_ARRAY, 9);
    for (unsigned i = 0; i < 9; i++) pb[n++] = 0x3F800000u;
    pb[n++] = method(0, NV097_SET_BEGIN_END, 1), pb[n++] = NV097_SET_BEGIN_END_OP_END;
    nv2a_w(NV2A_USER + NV_USER_DMA_PUT, n * 4);
    uint32_t get = 0;
    for (ULONGLONG t = GetTickCount64(); GetTickCount64() - t < 1000; Sleep(1))
        if ((get = nv2a_r(NV2A_USER + NV_USER_DMA_GET)) == n * 4) break;
    XbrtProbeCounts p1 = xbrt_probe_counts;
    XbrtCounters c1;
    xbrt_xemu_counters(&c1);
    uint64_t begins = p1.draw_begin - p0.draw_begin, ends = p1.draw_end - p0.draw_end;
    bool ok = p1.init == 1 && begins == 1 && ends == 1 && get == n * 4 && c1.nv2a_draws - c0.nv2a_draws == 1;
    RESULT(ok, "draw_begin %llu, draw_end %llu, get == put %d, draws counted %llu", (unsigned long long)begins,
           (unsigned long long)ends, get == n * 4, (unsigned long long)(c1.nv2a_draws - c0.nv2a_draws));
}

static Result nv2a_vblank_rate(void)
{
    nv2a_w(NV2A_PCRTC + NV_PCRTC_INTR_EN_0, NV_PCRTC_INTR_EN_0_VBLANK);
    bool rose = wait_level(LINE_NV2A, 1, 100);
    uint32_t pmc = nv2a_r(NV2A_PMC + NV_PMC_INTR_0);
    XbrtCounters c0, c1;
    xbrt_xemu_counters(&c0);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    Sleep(1000);
    xbrt_xemu_counters(&c1);
    QueryPerformanceCounter(&t1);
    nv2a_w(NV2A_PCRTC + NV_PCRTC_INTR_EN_0, 0);
    nv2a_w(NV2A_PCRTC + NV_PCRTC_INTR_0, NV_PCRTC_INTR_0_VBLANK);
    bool dropped = wait_level(LINE_NV2A, 0, 100);
    double rate = (double)(c1.vblanks - c0.vblanks) * (double)f.QuadPart / (double)(t1.QuadPart - t0.QuadPart);
    bool ok = rose && (pmc & NV_PMC_INTR_0_PCRTC) && dropped && rate > 57 && rate < 63;
    RESULT(ok, "line rose %d (PMC 0x%08X), %.2f vblanks per second, dropped %d", rose, pmc, rate, dropped);
}

static Result nv2a_ptimer_alarm(void)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    /* At reset PTIMER counts the 233 MHz core clock in units of 32: 0x00E00000 is about 2 ms. The alarm is armed
     * before it is enabled: reset leaves it at 0, which is already past. */
    nv2a_w(NV2A_PTIMER + NV_PTIMER_ALARM_0, nv2a_r(NV2A_PTIMER + NV_PTIMER_TIME_0) + 0x00E00000);
    nv2a_w(NV2A_PTIMER + NV_PTIMER_INTR_0, NV_PTIMER_INTR_0_ALARM);
    nv2a_w(NV2A_PTIMER + NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM);
    bool rose = wait_level(LINE_NV2A, 1, 200);
    QueryPerformanceCounter(&t1);
    uint32_t pmc = nv2a_r(NV2A_PMC + NV_PMC_INTR_0), intr = nv2a_r(NV2A_PTIMER + NV_PTIMER_INTR_0);
    nv2a_w(NV2A_PTIMER + NV_PTIMER_INTR_EN_0, 0);
    nv2a_w(NV2A_PTIMER + NV_PTIMER_INTR_0, NV_PTIMER_INTR_0_ALARM);
    bool dropped = wait_level(LINE_NV2A, 0, 100);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    bool ok = rose && pmc == NV_PMC_INTR_0_PTIMER && intr == NV_PTIMER_INTR_0_ALARM && dropped && ms > 1 && ms < 20;
    RESULT(ok, "line rose %d after %.1f ms (PMC 0x%08X, PTIMER 0x%08X), dropped %d", rose, ms, pmc, intr, dropped);
}

typedef struct Test {
    const char *name;
    Result (*run)(void);
} Test;

static const Test kTests[] = {
    {"flatten_and_dispatch", flatten_and_dispatch},
    {"read_masked_to_size", read_masked_to_size},
    {"hole_refuses", hole_refuses},
    {"unaligned_refuses", unaligned_refuses},
    {"ram_subregion_is_memory", ram_subregion_is_memory},
    {"map_change_after_freeze_refuses", map_change_after_freeze_refuses},
    {"bql_recursive_refuses", bql_recursive_refuses},
    {"timers_fire_in_order_with_bql", timers_fire_in_order_with_bql},
    {"assert_is_a_refusal", assert_is_a_refusal},
    {"irq_level_with_bql_and_isr_reenters", irq_level_with_bql_and_isr_reenters},
    {"muldiv64_wide", muldiv64_wide},
    {"apu_xgscnt_off_then_48k", apu_xgscnt_off_then_48k},
    {"nv2a_boot0_and_ram_bar", nv2a_boot0_and_ram_bar},
    {"nv2a_push_buffer_context_switch_semaphore", nv2a_push_buffer_context_switch_semaphore},
    {"probe_counts_one_draw", probe_counts_one_draw},
    {"nv2a_vblank_rate", nv2a_vblank_rate},
    {"nv2a_ptimer_alarm", nv2a_ptimer_alarm},
};

int main(void)
{
    g_test_thread = GetCurrentThreadId();
    g_irq_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_host.ram_bytes = 64u << 20;
    g_host.ram = VirtualAlloc(NULL, g_host.ram_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_host.voice_workers = 2;
    g_host.vblank_hz = 59.94;
    g_host.renderer = XBRT_RENDERER_PROBE;
    g_host.irq_level = test_irq_level;
    g_host.refuse = test_refuse;
    g_host.log = test_log;
    unsigned count = xbrt_machine_create(&g_host, kSpecs, ARRAY_SIZE(kSpecs), g_bars, ARRAY_SIZE(g_bars));
    for (unsigned i = 0; i < count; i++)
        printf("xemu bar %u: %s BAR%u size 0x%X line %u\n", i, g_bars[i].device, g_bars[i].index, g_bars[i].size,
               g_bars[i].irq_line);
    if (count != BAR_COUNT || strcmp(g_bars[BAR_APU].device, "APU") || g_bars[BAR_APU].irq_line != LINE_APU ||
        strcmp(g_bars[BAR_NV2A].device, "NV2A") || g_bars[BAR_NV2A_RAM].irq_line != LINE_NV2A) {
        printf("xemu tests: FAIL the machine has %u BARs, not the test device's two, the APU's and the NV2A's two\n",
               count);
        return 1;
    }
    xbrt_xemu_run();
    unsigned passed = 0, failed = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(kTests); i++) {
        Result r = kTests[i].run();
        printf("xemu test %s: %s %s\n", kTests[i].name, r.ok ? "pass" : "FAIL", r.detail);
        fflush(stdout);
        if (r.ok) passed++;
        else failed++;
    }
    printf("xemu tests: %u pass, %u fail\n", passed, failed);
    fflush(stdout);
    ExitProcess(failed ? 1 : 0);
}
