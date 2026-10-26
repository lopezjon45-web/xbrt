#include "../nv2a.h"

#define NV2A_BUS_LEVEL 3

static KINTERRUPT interrupt;
static KDPC dpc;
static volatile LONG isr_calls, context_switches, vblanks, alarms;

static BOOLEAN NTAPI isr(PKINTERRUPT i, PVOID context)
{
    (void)i;
    (void)context;
    if (!nv_rd(NV_PMC + PMC_INTR_0)) return FALSE;
    nv_wr(NV_PMC + PMC_INTR_EN_0, 0);
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
    ULONG pending = nv_rd(NV_PMC + PMC_INTR_0);
    if (pending & PMC_INTR_0_PGRAPH) {
        ULONG intr = nv_rd(NV_PGRAPH + PGRAPH_INTR);
        if (intr & PGRAPH_INTR_CONTEXT_SWITCH) {
            nv_wr(NV_PGRAPH + PGRAPH_CTX_USER, 0);
            nv_wr(NV_PGRAPH + PGRAPH_CTX_CONTROL, PGRAPH_CTX_CONTROL_CHID_VALID);
            context_switches++;
        }
        nv_wr(NV_PGRAPH + PGRAPH_INTR, intr);
    }
    if (pending & PMC_INTR_0_PCRTC) {
        nv_wr(NV_PCRTC + PCRTC_INTR_0, PCRTC_VBLANK);
        vblanks++;
    }
    if (pending & PMC_INTR_0_PTIMER) {
        nv_wr(NV_PTIMER + PTIMER_INTR_EN_0, 0);
        nv_wr(NV_PTIMER + PTIMER_INTR_0, PTIMER_ALARM);
        alarms++;
    }
    nv_wr(NV_PMC + PMC_INTR_EN_0, PMC_INTR_EN_0_HARDWARE);
}

static void fail(const char *what, ULONG value)
{
    LabLine s = lab_step(what);
    lab_put_hex(&s, value);
    lab_finish("nv2a", &s);
}

void __cdecl lab_entry(void)
{
    DbgPrint("lab nv2a start");
    lab_clock_init();
    LabLine boot0 = lab_step("lab nv2a boot0 ");
    lab_put_hex(&boot0, nv_rd(NV_PMC + PMC_BOOT_0));
    lab_print(&boot0);

    KIRQL irql;
    ULONG vector = HalGetInterruptVector(NV2A_BUS_LEVEL, &irql);
    KeInitializeDpc(&dpc, dpc_routine, NULL);
    KeInitializeInterrupt(&interrupt, isr, NULL, vector, irql, LevelSensitive, FALSE);
    KeConnectInterrupt(&interrupt);

    ULONG *pb = MmAllocateContiguousMemory(0x1000);
    volatile ULONG *semaphore = MmAllocateContiguousMemory(0x1000);
    if (!pb || !semaphore) fail("MmAllocateContiguousMemory returned ", 0);
    *semaphore = 0;
    ULONG n = 0;
    pb[n++] = nv_method(0, NV097_SET_OBJECT, 1), pb[n++] = H_KELVIN;
    pb[n++] = nv_method(0, NV097_SET_CONTEXT_DMA_SEMAPHORE, 1), pb[n++] = H_SEMAPHORE;
    pb[n++] = nv_method(0, NV097_SET_SEMAPHORE_OFFSET, 1), pb[n++] = 0;
    pb[n++] = nv_method(0, NV097_BACK_END_WRITE_SEMAPHORE_RELEASE, 1), pb[n++] = 0x00C0FFEE;
    while (n < 16) pb[n++] = nv_method(0, NV097_NO_OPERATION, 1), pb[n++] = 0;
    nv_channel_setup(MmGetPhysicalAddress(pb), 0xFFF, MmGetPhysicalAddress((PVOID)semaphore), NV_CONTEXT_SWITCH_PENDING);
    nv_wr(NV_PGRAPH + PGRAPH_INTR_EN, PGRAPH_INTR_CONTEXT_SWITCH);
    nv_wr(NV_PMC + PMC_INTR_EN_0, PMC_INTR_EN_0_HARDWARE);
    nv_wr(NV_USER + USER_DMA_PUT, n * 4);

    ULONGLONG start = KeQueryPerformanceCounter();
    ULONG get = 0;
    while ((get = nv_rd(NV_USER + USER_DMA_GET)) != n * 4 && lab_elapsed_ms(start) < 2000) {}
    if (context_switches != 1) fail("context switches handled: ", context_switches);
    DbgPrint("lab nv2a context switch interrupt handled");
    LabLine g = lab_step("lab nv2a get == put ");
    if (get != n * 4) fail("get stopped at ", get);
    lab_put_hex(&g, get);
    lab_print(&g);
    LabLine sem = lab_step("lab nv2a semaphore ");
    lab_put_hex(&sem, *semaphore);
    lab_print(&sem);

    nv_wr(NV_PCRTC + PCRTC_INTR_EN_0, PCRTC_VBLANK);
    start = KeQueryPerformanceCounter();
    while (lab_elapsed_ms(start) < 100) {}
    LONG v0 = vblanks;
    start = KeQueryPerformanceCounter();
    while (lab_elapsed_ms(start) < 1000) {}
    LONG per_second = vblanks - v0;
    nv_wr(NV_PCRTC + PCRTC_INTR_EN_0, 0);
    LabLine measured = lab_step("nv2a vblank measured ");
    lab_put_dec(&measured, (ULONG)per_second);
    lab_put(&measured, " per second");
    lab_print(&measured);
    if (per_second < 50 || per_second > 70) fail("vblanks in one second: ", (ULONG)per_second);
    DbgPrint("lab nv2a vblank 50..70 per second");

    // At reset PTIMER counts the 233 MHz core clock in units of 32: 0x00E00000 is about 2 ms. The alarm is armed
    // before it is enabled: reset leaves it at 0, which is already past.
    start = KeQueryPerformanceCounter();
    nv_wr(NV_PTIMER + PTIMER_ALARM_0, nv_rd(NV_PTIMER + PTIMER_TIME_0) + 0x00E00000u);
    nv_wr(NV_PTIMER + PTIMER_INTR_0, PTIMER_ALARM);
    nv_wr(NV_PTIMER + PTIMER_INTR_EN_0, PTIMER_ALARM);
    while (!alarms && lab_elapsed_ms(start) < 100) {}
    ULONG ms = lab_elapsed_ms(start);
    LabLine at = lab_step("nv2a ptimer alarm after ");
    lab_put_dec(&at, ms);
    lab_put(&at, " ms");
    lab_print(&at);
    if (alarms != 1 || ms < 1) fail("ptimer alarms, then ms: ", alarms << 16 | ms);
    DbgPrint("lab nv2a ptimer alarm");
    lab_finish("nv2a", NULL);
}
