#pragma once

#include <xboxkrnl/xboxkrnl.h>

typedef struct {
    char text[160];
    unsigned len;
} LabLine;

static inline LabLine lab_line(void)
{
    LabLine l;
    l.len = 0;
    l.text[0] = 0;
    return l;
}

static inline void lab_put(LabLine *l, const char *s)
{
    while (*s && l->len < sizeof l->text - 1) l->text[l->len++] = *s++;
    l->text[l->len] = 0;
}

static inline void lab_put_hex(LabLine *l, ULONG v)
{
    char digits[11] = "0x";
    for (int i = 0; i < 8; i++) digits[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 0xF];
    digits[10] = 0;
    lab_put(l, digits);
}

static inline void lab_put_dec(LabLine *l, ULONG v)
{
    char digits[11];
    int n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char out[11];
    for (int i = 0; i < n; i++) out[i] = digits[n - 1 - i];
    out[n] = 0;
    lab_put(l, out);
}

static inline void lab_print(const LabLine *l) { DbgPrint(l->text); }

static inline void __attribute__((noreturn)) lab_finish(const char *name, const LabLine *step)
{
    LabLine l = lab_line();
    lab_put(&l, "lab ");
    lab_put(&l, name);
    if (step) {
        lab_put(&l, " FAIL ");
        lab_put(&l, step->text);
    } else {
        lab_put(&l, " ok");
    }
    lab_print(&l);
    HalReturnToFirmware(HalQuickRebootRoutine);
}

static inline LabLine lab_step(const char *what)
{
    LabLine l = lab_line();
    lab_put(&l, what);
    return l;
}

static inline LabLine lab_step_status(const char *what, NTSTATUS status)
{
    LabLine l = lab_step(what);
    lab_put(&l, " ");
    lab_put_hex(&l, (ULONG)status);
    return l;
}

static inline unsigned lab_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}


// NtCreateFile for synchronous I/O, relative to `root` when it is not NULL.
static inline NTSTATUS lab_open(HANDLE *file, HANDLE root, const char *path, ACCESS_MASK access, ULONG disposition,
                                ULONG options)
{
    ANSI_STRING name;
    name.Buffer = (PCHAR)path;
    name.Length = (USHORT)lab_strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    OBJECT_ATTRIBUTES attributes;
    attributes.RootDirectory = root;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK io;
    return NtCreateFile(file, access | SYNCHRONIZE, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, disposition, options | FILE_SYNCHRONOUS_IO_NONALERT);
}
