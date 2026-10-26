#include "../lab.h"

// Register values are upstream's apu_regs.h.

#define APU 0xFE800000u
#define ISTS 0x1000u
#define IEN 0x1004u
#define FECTL 0x1100u
#define SECTL 0x2000u
#define XGSCNT 0x200Cu
#define TVL2D 0x2054u
#define TVL3D 0x2060u
#define TVLMP 0x206Cu
#define VOICE_LIST_EMPTY 0xFFFFu
#define ISTS_GINTSTS 0x01u
#define ISTS_FETINTSTS 0x10u
#define FECTL_TRAPPED 0xE0u
#define SECTL_XCNTMODE_ON 0x08u
#define APU_BUS_LEVEL 5

static ULONG rd(ULONG offset) { return *(volatile ULONG *)(APU + offset); }
static void wr(ULONG offset, ULONG value) { *(volatile ULONG *)(APU + offset) = value; }

static KINTERRUPT interrupt;
static KDPC dpc;
static volatile LONG isr_calls, dpc_calls;
static volatile ULONG isr_ists;
static PKTHREAD volatile isr_thread;

static BOOLEAN NTAPI isr(PKINTERRUPT i, PVOID context)
{
    (void)i;
    (void)context;
    ULONG ists = rd(ISTS);
    if (!(ists & ISTS_FETINTSTS)) return FALSE;
    isr_ists = ists;
    isr_thread = KeGetCurrentThread();
    wr(FECTL, 0);
    wr(ISTS, ISTS_FETINTSTS | ISTS_GINTSTS);
    isr_calls++;
    KeInsertQueueDpc(&dpc, NULL, NULL);
    return TRUE;
}

static void NTAPI dpc_routine(PKDPC d, PVOID context, PVOID a1, PVOID a2)
{
    (void)d;
    (void)context;
    (void)a1;
    (void)a2;
    dpc_calls++;
}

// Milliseconds in 32-bit arithmetic: the lab links no CRT, so a 64-bit division (__aulldiv) has no body. Spans stay
// under 5 s, which fits 32 bits of the performance counter.
static ULONG freq_khz;

static ULONG elapsed_ms(ULONGLONG since) { return (ULONG)(KeQueryPerformanceCounter() - since) / freq_khz; }

static void spin_ms(ULONG ms)
{
    ULONGLONG start = KeQueryPerformanceCounter();
    while (elapsed_ms(start) < ms) {}
}

static void fail_counts(const char *what, ULONG a, ULONG b)
{
    LabLine s = lab_step(what);
    lab_put_dec(&s, a);
    lab_put(&s, " then ");
    lab_put_dec(&s, b);
    lab_finish("apu", &s);
}

void __cdecl lab_entry(void)
{
    DbgPrint("lab apu start");
    freq_khz = (ULONG)KeQueryPerformanceFrequency() / 1000u;

    ULONG off0 = rd(XGSCNT);
    spin_ms(20);
    ULONG off1 = rd(XGSCNT);
    if (off1 != off0) fail_counts("xgscnt moved with xcntmode off: ", off0, off1);
    DbgPrint("lab apu xgscnt still while xcntmode off");

    // Reset leaves each voice list's top at voice 0; a driver empties them before it starts the frame clock, or the
    // first frame idles voice 0 and upstream refuses.
    wr(TVL2D, VOICE_LIST_EMPTY);
    wr(TVL3D, VOICE_LIST_EMPTY);
    wr(TVLMP, VOICE_LIST_EMPTY);
    wr(SECTL, SECTL_XCNTMODE_ON);
    spin_ms(20);
    ULONG c0 = rd(XGSCNT);
    ULONGLONG t0 = KeQueryPerformanceCounter();
    spin_ms(500);
    ULONG c1 = rd(XGSCNT);
    ULONG ms = elapsed_ms(t0);
    if (c1 == c0) fail_counts("xgscnt stuck: ", c0, c1);
    DbgPrint("lab apu xgscnt advances");
    ULONG rate = (c1 - c0) * 1000u / ms;
    LabLine measured = lab_step("apu xgscnt measured ");
    lab_put_dec(&measured, rate);
    lab_put(&measured, " samples per second");
    lab_print(&measured);
    if (rate < 40000 || rate > 56000) fail_counts("xgscnt rate outside 40000..56000: samples ", c1 - c0, rate);
    DbgPrint("lab apu xgscnt rate 48k");

    KIRQL irql;
    ULONG vector = HalGetInterruptVector(APU_BUS_LEVEL, &irql);
    KeInitializeDpc(&dpc, dpc_routine, NULL);
    KeInitializeInterrupt(&interrupt, isr, NULL, vector, irql, LevelSensitive, FALSE);
    KeConnectInterrupt(&interrupt);
    wr(IEN, ISTS_GINTSTS | ISTS_FETINTSTS);
    wr(FECTL, FECTL_TRAPPED);
    ULONGLONG raised = KeQueryPerformanceCounter();
    while ((!isr_calls || !dpc_calls) && elapsed_ms(raised) < 1000) {}
    if (!isr_calls || isr_thread == KeGetCurrentThread()) fail_counts("isr calls, on this thread: ", isr_calls, isr_thread == KeGetCurrentThread());
    LabLine saw = lab_step("lab apu isr saw ists ");
    lab_put_hex(&saw, isr_ists);
    lab_print(&saw);
    spin_ms(50);
    if (isr_calls != 1 || dpc_calls != 1) fail_counts("isr calls then dpc calls: ", isr_calls, dpc_calls);
    DbgPrint("lab apu dpc ran");
    lab_finish("apu", NULL);
}
