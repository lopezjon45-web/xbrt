#include "../lab.h"

static const LONGLONG kTimerThreadTicks = -500000;

static KTIMER timer;
static KDPC dpc;
static volatile LONG helper_ran;

static VOID NTAPI never_due(PKDPC d, PVOID context, PVOID a1, PVOID a2) {}

// XAPI's debug monitor notification: KPRCB+0x250 names the monitor, whose +0x14 is called with two stdcall
// arguments. Returns how far esp moved across it, ~0 when there is no monitor.
static ULONG debug_monitor_notify(void)
{
    ULONG before, after;
    __asm__ volatile("movl %%esp, %0\n\t"
                     "movl %%fs:0x20, %%eax\n\t"
                     "movl 0x250(%%eax), %%eax\n\t"
                     "testl %%eax, %%eax\n\t"
                     "jz 1f\n\t"
                     "pushl $0\n\t"
                     "pushl $0xa\n\t"
                     "calll *0x14(%%eax)\n\t"
                     "movl %%esp, %1\n\t"
                     "jmp 2f\n\t"
                     "1:\n\t"
                     "movl $0, %1\n\t"
                     "2:"
                     : "=&r"(before), "=&r"(after)
                     :
                     : "eax", "ecx", "edx", "memory");
    return after ? before - after : ~0u;
}

static VOID NTAPI helper(PVOID context) { helper_ran = (LONG)(ULONG_PTR)context; }

static VOID NTAPI title_main(PVOID context)
{
    DbgPrint("lab startup main thread");
    ULONG moved = debug_monitor_notify();
    if (moved) {
        LabLine s = lab_step("debug monitor notification moved esp by ");
        lab_put_hex(&s, moved);
        lab_finish("startup", &s);
    }
    HANDLE thread;
    NTSTATUS status = PsCreateSystemThreadEx(&thread, 0, 0x4000, 0, NULL, helper, (PVOID)7, FALSE, FALSE, NULL);
    if (status != STATUS_SUCCESS) {
        LabLine s = lab_step_status("PsCreateSystemThreadEx with no system routine", status);
        lab_finish("startup", &s);
    }
    LARGE_INTEGER limit;
    limit.QuadPart = -20000000;
    status = NtWaitForSingleObject(thread, FALSE, &limit);
    NtClose(thread);
    if (status != STATUS_SUCCESS || helper_ran != 7) {
        LabLine s = lab_step_status("the start routine of a thread with no system routine did not run: wait", status);
        lab_finish("startup", &s);
    }

    KeInitializeDpc(&dpc, never_due, NULL);
    KeInitializeTimerEx(&timer, NotificationTimer);
    LARGE_INTEGER due;
    due.QuadPart = -36000000000ll;
    KeSetTimer(&timer, due, &dpc);
    LARGE_INTEGER pause;
    pause.QuadPart = kTimerThreadTicks;
    KeDelayExecutionThread(KernelMode, FALSE, &pause);

    ANSI_STRING name;
    name.Buffer = "\\Device\\Harddisk0\\partition1\\TDATA";
    name.Length = sizeof "\\Device\\Harddisk0\\partition1\\TDATA" - 1;
    name.MaximumLength = sizeof "\\Device\\Harddisk0\\partition1\\TDATA";
    OBJECT_ATTRIBUTES attributes;
    attributes.RootDirectory = NULL;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK io;
    HANDLE folder;
    status = NtCreateFile(&folder, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (status != STATUS_SUCCESS) {
        LabLine s = lab_step_status("NtCreateFile partition1\\TDATA", status);
        lab_finish("startup", &s);
    }
    NtClose(folder);
    KeCancelTimer(&timer);
    lab_finish("startup", NULL);
}

static VOID NTAPI system_start(PKSTART_ROUTINE routine, PVOID context) { routine(context); }

void __cdecl lab_entry(void)
{
    DbgPrint("lab startup start");
    HANDLE thread, id;
    NTSTATUS status = PsCreateSystemThreadEx(&thread, 0, 0x10000, 0, &id, title_main, NULL, FALSE, FALSE, system_start);
    if (status != STATUS_SUCCESS) {
        LabLine s = lab_step_status("PsCreateSystemThreadEx", status);
        lab_finish("startup", &s);
    }
    NtClose(thread);
}
