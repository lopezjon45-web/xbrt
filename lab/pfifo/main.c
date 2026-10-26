#include "../nv2a.h"

// PFIFO under load. The guest writes push-buffer words with ordinary stores, then stores DMA_PUT, which traps; PFIFO
// must never read a word the guest had not finished writing and never miss a kick. 20000 kicks of one 16-byte packet
// (SET_ZSTENCIL_CLEAR_VALUE, then SET_COLOR_CLEAR_VALUE carrying the kick number), even kicks written with four plain
// stores and odd ones with one non-temporal store, while a second guest thread reads PFIFO and PGRAPH registers the
// whole time. A stale word shows as a colour clear value that is not the kick's. The non-temporal kicks can falsify
// the assumption that the trap drains a write-combining buffer before PFIFO reads, never prove it.

#define KICKS 20000u
#define RING 0x10000u
#define PACKET 16u
#define CHECK_EVERY 256u
#define READER_BURST 64u

static volatile LONG stop_reader, reads, bursts;

// Guest threads share one core, and the runner's quantum outlasts the whole kick loop, so the two threads take
// turns at yields; the PFIFO thread runs on another core throughout.
static VOID NTAPI reader(PVOID context)
{
    (void)context;
    while (!stop_reader) {
        for (ULONG k = 0; k < READER_BURST; k++) {
            nv_rd(NV_PFIFO + PFIFO_CACHE1_DMA_GET);
            nv_rd(NV_PGRAPH + PGRAPH_INTR);
            reads++;
        }
        bursts++;
        NtYieldExecution();
    }
}

static void put_packet(ULONG *p, ULONG value, int non_temporal)
{
    __attribute__((aligned(16))) ULONG packet[4] = {nv_method(0, NV097_SET_ZSTENCIL_CLEAR_VALUE, 1), 0,
                                                    nv_method(0, NV097_SET_COLOR_CLEAR_VALUE, 1), value};
    if (non_temporal)
        __asm__ volatile("movaps (%1), %%xmm0\n\tmovntps %%xmm0, (%0)" ::"r"(p), "r"(packet) : "xmm0", "memory");
    else
        for (int k = 0; k < 4; k++) ((volatile ULONG *)p)[k] = packet[k];
}

static void fail(const char *what, ULONG a, ULONG b)
{
    LabLine s = lab_step(what);
    lab_put_dec(&s, a);
    lab_put(&s, " ");
    lab_put_hex(&s, b);
    lab_finish("pfifo", &s);
}

static void drain(ULONG put, ULONG kick)
{
    ULONGLONG start = KeQueryPerformanceCounter();
    ULONG get;
    while ((get = nv_rd(NV_USER + USER_DMA_GET)) != put)
        if (lab_elapsed_ms(start) > 2000) fail("never drained: kick, get ", kick, get);
}

void __cdecl lab_entry(void)
{
    DbgPrint("lab pfifo start");
    lab_clock_init();
    ULONG *ring = MmAllocateContiguousMemory(RING);
    PVOID semaphore = MmAllocateContiguousMemory(0x1000);
    if (!ring || !semaphore) fail("MmAllocateContiguousMemory: ", 0, 0);
    nv_channel_setup(MmGetPhysicalAddress(ring), RING - 1, MmGetPhysicalAddress(semaphore), 1);
    // Sixteen bytes, so every packet after it is 16-byte aligned for the non-temporal store.
    ring[0] = nv_method(0, NV097_SET_OBJECT, 1);
    ring[1] = H_KELVIN;
    ring[2] = nv_method(0, NV097_NO_OPERATION, 1);
    ring[3] = 0;
    nv_wr(NV_USER + USER_DMA_PUT, PACKET);
    drain(PACKET, 0);
    DbgPrint("lab pfifo channel up");

    HANDLE thread;
    NTSTATUS status = PsCreateSystemThreadEx(&thread, 0, 0x4000, 0, NULL, reader, NULL, FALSE, FALSE, NULL);
    if (!NT_SUCCESS(status)) fail("PsCreateSystemThreadEx: ", 0, (ULONG)status);

    ULONG put = PACKET;
    for (ULONG i = 0; i < KICKS; i++) {
        if (put + PACKET + 4 > RING) {
            ring[put / 4] = 1;   // a jump to offset 0
            put = 0;
            nv_wr(NV_USER + USER_DMA_PUT, put);
            drain(put, i);
        }
        put_packet(ring + put / 4, i, i & 1);
        put += PACKET;
        nv_wr(NV_USER + USER_DMA_PUT, put);
        if (i % CHECK_EVERY == CHECK_EVERY - 1) {
            NtYieldExecution();
            drain(put, i);
            ULONG applied = nv_rd(NV_PGRAPH + PGRAPH_COLORCLEARVALUE);
            if (applied != i) fail("stale packet: kick, colour clear value ", i, applied);
        }
    }
    drain(put, KICKS);
    stop_reader = 1;
    LARGE_INTEGER limit;
    limit.QuadPart = -20000000;
    NtWaitForSingleObject(thread, FALSE, &limit);
    NtClose(thread);
    LabLine drained = lab_step("lab pfifo ");
    lab_put_dec(&drained, KICKS);
    lab_put(&drained, " kicks drained");
    lab_print(&drained);
    ULONG last = nv_rd(NV_PGRAPH + PGRAPH_COLORCLEARVALUE);
    if (last != KICKS - 1) fail("last packet: colour clear value ", KICKS - 1, last);
    DbgPrint("lab pfifo last method applied");
    LabLine r = lab_step("pfifo reader made ");
    lab_put_dec(&r, (ULONG)reads);
    lab_put(&r, " register reads in ");
    lab_put_dec(&r, (ULONG)bursts);
    lab_put(&r, " turns");
    lab_print(&r);
    if (bursts < KICKS / CHECK_EVERY / 2) fail("the reader ran between too few kicks: bursts, reads ", bursts, reads);
    lab_finish("pfifo", NULL);
}
