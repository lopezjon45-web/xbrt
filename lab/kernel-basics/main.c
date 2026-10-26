#include "../lab.h"

static const char kExpected[] = "xbrt lab: this file was read from D:\\lab.txt\n";

static const ULONGLONG kNtTime2020 = 132223104000000000ull;

void __cdecl lab_entry(void)
{
    DbgPrint("lab kernel-basics start");

    LARGE_INTEGER t0, t1;
    KeQuerySystemTime(&t0);
    KeQuerySystemTime(&t1);
    if ((ULONGLONG)t0.QuadPart < kNtTime2020) {
        LabLine s = lab_step("KeQuerySystemTime before 2020 ");
        lab_put_hex(&s, (ULONG)(t0.QuadPart >> 32));
        lab_finish("kernel-basics", &s);
    }
    if (t1.QuadPart < t0.QuadPart) {
        LabLine s = lab_step("KeQuerySystemTime went backwards");
        lab_finish("kernel-basics", &s);
    }

    ANSI_STRING name;
    name.Buffer = "\\??\\D:\\lab.txt";
    name.Length = sizeof "\\??\\D:\\lab.txt" - 1;
    name.MaximumLength = sizeof "\\??\\D:\\lab.txt";
    OBJECT_ATTRIBUTES attributes;
    attributes.RootDirectory = NULL;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK io;
    HANDLE file;
    NTSTATUS status = NtCreateFile(&file, GENERIC_READ | SYNCHRONIZE, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                                   FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS) {
        LabLine s = lab_step_status("NtCreateFile D:\\lab.txt", status);
        lab_finish("kernel-basics", &s);
    }

    char buffer[128];
    LARGE_INTEGER offset;
    offset.QuadPart = 0;
    status = NtReadFile(file, NULL, NULL, NULL, &io, buffer, sizeof buffer, &offset);
    if (status != STATUS_SUCCESS) {
        LabLine s = lab_step_status("NtReadFile", status);
        lab_finish("kernel-basics", &s);
    }
    if (io.Information != sizeof kExpected - 1) {
        LabLine s = lab_step("NtReadFile read ");
        lab_put_dec(&s, io.Information);
        lab_put(&s, " bytes, the file holds ");
        lab_put_dec(&s, sizeof kExpected - 1);
        lab_finish("kernel-basics", &s);
    }
    for (unsigned i = 0; i < sizeof kExpected - 1; i++) {
        if (buffer[i] != kExpected[i]) {
            LabLine s = lab_step("NtReadFile byte ");
            lab_put_dec(&s, i);
            lab_put(&s, " differs");
            lab_finish("kernel-basics", &s);
        }
    }

    status = NtClose(file);
    if (status != STATUS_SUCCESS) {
        LabLine s = lab_step_status("NtClose", status);
        lab_finish("kernel-basics", &s);
    }
    lab_finish("kernel-basics", NULL);
}
