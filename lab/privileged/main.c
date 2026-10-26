#include "../lab.h"

#define LOOPBACK_PORT 0x80
#define GUEST_TSC_TICKS_PER_3_US 2200
#define XBOX_CLOCK_TICK_US 1000

static ULONGLONG rdtsc(void)
{
    ULONG lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((ULONGLONG)hi << 32) | lo;
}

void __cdecl lab_entry(void)
{
    DbgPrint("lab privileged start");

    __asm__ volatile("wbinvd" ::: "memory");
    __asm__ volatile("cli\n"
                     "sti" ::: "memory");

    ULONG byte_back;
    __asm__ volatile("movb $0x5A, %%al\n"
                     "outb %%al, %1\n"
                     "xorl %%eax, %%eax\n"
                     "inb %1, %%al"
                     : "=&a"(byte_back)
                     : "i"(LOOPBACK_PORT));
    if (byte_back != 0x5A) {
        LabLine s = lab_step("in al,0x80 after out 0x80,0x5A read ");
        lab_put_hex(&s, byte_back);
        lab_finish("privileged", &s);
    }
    ULONG dword_back;
    __asm__ volatile("outl %%eax, %%dx\n"
                     "xorl %%eax, %%eax\n"
                     "inl %%dx, %%eax"
                     : "=a"(dword_back)
                     : "a"(0xC0FFEE11u), "d"(LOOPBACK_PORT));
    if (dword_back != 0xC0FFEE11u) {
        LabLine s = lab_step("in eax,dx after out dx,0xC0FFEE11 read ");
        lab_put_hex(&s, dword_back);
        lab_finish("privileged", &s);
    }

    LARGE_INTEGER s0, s1, interval;
    interval.QuadPart = -1000000;
    KeQuerySystemTime(&s0);
    ULONGLONG t0 = rdtsc();
    NTSTATUS status = KeDelayExecutionThread(KernelMode, FALSE, &interval);
    ULONGLONG t1 = rdtsc();
    KeQuerySystemTime(&s1);
    if (status != STATUS_SUCCESS) {
        LabLine s = lab_step_status("KeDelayExecutionThread", status);
        lab_finish("privileged", &s);
    }
    ULONGLONG ticks = t1 - t0;
    LONGLONG system = s1.QuadPart - s0.QuadPart;
    if (t1 <= t0 || (ticks >> 32) || system <= 0 || (system >> 32)) {
        LabLine s = lab_step("rdtsc or KeQuerySystemTime delta out of range around a 100 ms delay");
        lab_finish("privileged", &s);
    }
    ULONG tsc_us = (ULONG)ticks / GUEST_TSC_TICKS_PER_3_US * 3;
    ULONG system_us = (ULONG)system / 10;
    if (system_us < 99000 || system_us > 1000000) {
        LabLine s = lab_step("KeDelayExecutionThread of 100 ms took systime_us=");
        lab_put_dec(&s, system_us);
        lab_finish("privileged", &s);
    }
    ULONG off = tsc_us > system_us ? tsc_us - system_us : system_us - tsc_us;
    ULONG one_percent = system_us / 100;
    if (off > one_percent + XBOX_CLOCK_TICK_US) {
        LabLine s = lab_step("rdtsc tsc_us=");
        lab_put_dec(&s, tsc_us);
        lab_put(&s, " systime_us=");
        lab_put_dec(&s, system_us);
        lab_finish("privileged", &s);
    }
    lab_finish("privileged", NULL);
}
