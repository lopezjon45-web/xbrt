#include "../lab.h"

// An XDK title's start: the entry creates the title's main thread through a system routine, as XAPI's does, and
// returns; that thread arms a timer and opens the hard disk's title-data folder before the title proper runs.

static KTIMER timer;
static KDPC dpc;
static volatile LONG helper_ran;

static VOID NTAPI never_due(PKDPC d, PVOID context, PVOID a1, PVOID a2) {}

static VOID NTAPI helper(PVOID context) { helper_ran = (LONG)(ULONG_PTR)context; }

static VOID NTAPI title_main(PVOID context)
{
    DbgPrint("lab startup main thread");

    // With no system routine the kernel calls the start routine itself.
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
    // Long enough for the kernel's timer thread, started by KeSetTimer, to tick many times.
    LARGE_INTEGER pause;
    pause.QuadPart = -500000;
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
