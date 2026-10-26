#include "../lab.h"

#include <excpt.h>

#define UNMAPPED 0x00000004u
#define STATUS_ACCESS_VIOLATION_CODE 0xC0000005u

static __attribute__((noinline)) void write_unmapped(void) { *(volatile ULONG *)UNMAPPED = 1; }

static ULONG fs0(void)
{
    ULONG v;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(v));
    return v;
}

static int caught_by_try(ULONG *code)
{
    int caught = 0;
    __try {
        write_unmapped();
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        caught = 1;
    }
    return caught;
}

#define RAISED_CODE 0xE0584252u

// Two __finally blocks between the fault and the __except that takes it: the unwind runs both, innermost first.
static volatile ULONG finally_order;

static __attribute__((noinline)) void inner_finally(void)
{
    __try {
        write_unmapped();
    } __finally {
        finally_order = finally_order * 10 + 1;
    }
}

static __attribute__((noinline)) void outer_finally(void)
{
    __try {
        inner_finally();
    } __finally {
        finally_order = finally_order * 10 + 2;
    }
}

static int caught_through_finally(void)
{
    int caught = 0;
    __try {
        outer_finally();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        caught = 1;
    }
    return caught;
}

// A filter that faults where no frame of its own takes it: the second exception is nested in the first, its dispatch
// passes over the frame whose filter is running, and the outermost __except takes it.
static volatile int nested_filter_ran;

static int faulting_filter(void)
{
    nested_filter_ran = 1;
    write_unmapped();
    return EXCEPTION_EXECUTE_HANDLER;
}

static __attribute__((noinline)) void fault_with_faulting_filter(void)
{
    __try {
        write_unmapped();
    } __except (faulting_filter()) {
    }
}

static int caught_nested(ULONG *code)
{
    int caught = 0;
    __try {
        fault_with_faulting_filter();
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        caught = 1;
    }
    return caught;
}

static int caught_raise(ULONG *code, ULONG *parameter)
{
    int caught = 0;
    __try {
        EXCEPTION_RECORD r = {0};
        r.ExceptionCode = (NTSTATUS)RAISED_CODE;
        r.NumberParameters = 1;
        r.ExceptionInformation[0] = 0x5EB;
        RtlRaiseException(&r);
    } __except (*code = GetExceptionCode(),
                *parameter = GetExceptionInformation()->ExceptionRecord->ExceptionInformation[0],
                EXCEPTION_EXECUTE_HANDLER) {
        caught = 1;
    }
    return caught;
}

int __cdecl raw_seh_probe(void);
extern char raw_seh_resume[];
__asm__(".text\n"
        ".globl _raw_seh_probe\n"
        "_raw_seh_probe:\n"
        "    pushl %ebx\n"
        "    pushl $_raw_handler\n"
        "    .byte 0x64, 0xFF, 0x35, 0x00, 0x00, 0x00, 0x00\n"   // push dword fs:[0]
        "    .byte 0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00\n"   // mov fs:[0],esp
        "    xorl %ebx, %ebx\n"
        "    movl $4, %eax\n"
        "    movl $1, (%eax)\n"
        ".globl _raw_seh_resume\n"
        "_raw_seh_resume:\n"
        "    .byte 0x64, 0x8F, 0x05, 0x00, 0x00, 0x00, 0x00\n"   // pop dword fs:[0]
        "    addl $4, %esp\n"
        "    movl %ebx, %eax\n"
        "    popl %ebx\n"
        "    ret\n");

// The same frame with a handler that does what the C++ runtime's catch does before it jumps to the catch block: it
// keeps the head of fs:[0], unwinds to its own frame, then links the kept head back on top of that frame.
int __cdecl relink_seh_probe(void);
extern char relink_seh_resume[];
__asm__(".text\n"
        ".globl _relink_seh_probe\n"
        "_relink_seh_probe:\n"
        "    pushl %ebx\n"
        "    pushl $_relink_handler\n"
        "    .byte 0x64, 0xFF, 0x35, 0x00, 0x00, 0x00, 0x00\n"   // push dword fs:[0]
        "    .byte 0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00\n"   // mov fs:[0],esp
        "    xorl %ebx, %ebx\n"
        "    movl $4, %eax\n"
        "    movl $1, (%eax)\n"
        ".globl _relink_seh_resume\n"
        "_relink_seh_resume:\n"
        "    .byte 0x64, 0x8F, 0x05, 0x00, 0x00, 0x00, 0x00\n"   // pop dword fs:[0]
        "    addl $4, %esp\n"
        "    movl %ebx, %eax\n"
        "    popl %ebx\n"
        "    ret\n");

static ULONG relink_head, relink_after_unwind, relink_frame;

EXCEPTION_DISPOSITION __cdecl relink_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context, void *dispatcher)
{
    EXCEPTION_REGISTRATION *head = (EXCEPTION_REGISTRATION *)fs0();
    relink_head = (ULONG)head;
    relink_frame = (ULONG)frame;
    RtlUnwind(frame, 0, record, 0);
    relink_after_unwind = fs0();
    head->prev = (EXCEPTION_REGISTRATION *)fs0();
    __asm__ volatile("movl %0, %%fs:0" : : "r"(head) : "memory");
    context->Eip = (ULONG)relink_seh_resume;
    context->Ebx = 1;
    return ExceptionContinueExecution;
}

static ULONG raw_code, raw_address, raw_parameters;

EXCEPTION_DISPOSITION __cdecl raw_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context, void *dispatcher)
{
    raw_code = (ULONG)record->ExceptionCode;
    raw_parameters = record->NumberParameters;
    raw_address = record->NumberParameters >= 2 ? record->ExceptionInformation[1] : 0;
    context->Eip = (ULONG)raw_seh_resume;
    context->Ebx = 1;
    return ExceptionContinueExecution;
}

void __cdecl lab_entry(void)
{
    DbgPrint("lab seh start");
    ULONG before = fs0();

    ULONG code = 0;
    if (!caught_by_try(&code)) {
        LabLine s = lab_step("__except did not run");
        lab_finish("seh", &s);
    }
    if (code != STATUS_ACCESS_VIOLATION_CODE) {
        LabLine s = lab_step("__except saw code ");
        lab_put_hex(&s, code);
        lab_finish("seh", &s);
    }
    if (fs0() != before) {
        LabLine s = lab_step("fs:[0] after __try is ");
        lab_put_hex(&s, fs0());
        lab_put(&s, ", was ");
        lab_put_hex(&s, before);
        lab_finish("seh", &s);
    }

    if (!caught_through_finally() || finally_order != 12) {
        LabLine s = lab_step("__finally blocks ran in the order ");
        lab_put_dec(&s, finally_order);
        lab_put(&s, ", want 12");
        lab_finish("seh", &s);
    }
    if (fs0() != before) {
        LabLine s = lab_step("fs:[0] after the __finally unwind is ");
        lab_put_hex(&s, fs0());
        lab_finish("seh", &s);
    }

    ULONG nested_code = 0;
    if (!caught_nested(&nested_code) || !nested_filter_ran || nested_code != STATUS_ACCESS_VIOLATION_CODE) {
        LabLine s = lab_step("a fault in a filter was not taken by the outer __except: code ");
        lab_put_hex(&s, nested_code);
        lab_finish("seh", &s);
    }
    if (fs0() != before) {
        LabLine s = lab_step("fs:[0] after the nested fault is ");
        lab_put_hex(&s, fs0());
        lab_finish("seh", &s);
    }

    ULONG raised = 0, parameter = 0;
    if (!caught_raise(&raised, &parameter)) {
        LabLine s = lab_step("__except did not run for RtlRaiseException");
        lab_finish("seh", &s);
    }
    if (raised != RAISED_CODE || parameter != 0x5EB) {
        LabLine s = lab_step("__except saw raised code ");
        lab_put_hex(&s, raised);
        lab_put(&s, " parameter ");
        lab_put_hex(&s, parameter);
        lab_finish("seh", &s);
    }
    if (fs0() != before) {
        LabLine s = lab_step("fs:[0] after the raise is ");
        lab_put_hex(&s, fs0());
        lab_finish("seh", &s);
    }

    if (raw_seh_probe() != 1) {
        LabLine s = lab_step("raw frame handler did not resume");
        lab_finish("seh", &s);
    }
    if (raw_code != STATUS_ACCESS_VIOLATION_CODE || raw_parameters < 2 || raw_address != UNMAPPED) {
        LabLine s = lab_step("raw frame saw code ");
        lab_put_hex(&s, raw_code);
        lab_put(&s, " address ");
        lab_put_hex(&s, raw_address);
        lab_finish("seh", &s);
    }
    if (fs0() != before) {
        LabLine s = lab_step("fs:[0] after the raw frame is ");
        lab_put_hex(&s, fs0());
        lab_finish("seh", &s);
    }
    if (relink_seh_probe() != 1) {
        LabLine s = lab_step("relinking handler did not resume");
        lab_finish("seh", &s);
    }
    if (relink_head == relink_frame || relink_after_unwind != relink_frame) {
        LabLine s = lab_step("the handler ran with fs:[0] at ");
        lab_put_hex(&s, relink_head);
        lab_put(&s, ", its frame ");
        lab_put_hex(&s, relink_frame);
        lab_put(&s, ", after RtlUnwind ");
        lab_put_hex(&s, relink_after_unwind);
        lab_finish("seh", &s);
    }
    if (fs0() != before) {
        LabLine s = lab_step("fs:[0] after the relinking frame is ");
        lab_put_hex(&s, fs0());
        lab_finish("seh", &s);
    }
    lab_finish("seh", NULL);
}
