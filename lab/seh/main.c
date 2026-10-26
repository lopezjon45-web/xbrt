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
    lab_finish("seh", NULL);
}
