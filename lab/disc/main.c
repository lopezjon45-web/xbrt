#include "../lab.h"

// D:\ as a title reads it: reads at an offset (across a 64 KiB cache block and over several blocks), a read at the
// file pointer, the end of a file, a directory listing with and without a pattern, an open relative to a directory,
// full attributes, and the status a missing file and a missing folder get. The same lines come from an extracted
// folder and from an XISO of it. media\ramp.bin is make-disc.py's: byte i is (i * 7 + (i >> 8)) & 0xFF.

#define RAMP_BYTES 200000u

static unsigned char g_buffer[3 * 65536 + 4096];

static unsigned char ramp(ULONG i) { return (unsigned char)(i * 7 + (i >> 8)); }

static void fail_status(const char *what, NTSTATUS status)
{
    LabLine s = lab_step_status(what, status);
    lab_finish("disc", &s);
}

static void check_ramp(const char *what, ULONG offset, ULONG length, IO_STATUS_BLOCK *io)
{
    if (io->Information != length) {
        LabLine s = lab_step(what);
        lab_put(&s, " read ");
        lab_put_dec(&s, io->Information);
        lab_put(&s, " bytes of ");
        lab_put_dec(&s, length);
        lab_finish("disc", &s);
    }
    for (ULONG i = 0; i < length; i++) {
        if (g_buffer[i] != ramp(offset + i)) {
            LabLine s = lab_step(what);
            lab_put(&s, " byte ");
            lab_put_dec(&s, offset + i);
            lab_put(&s, " is ");
            lab_put_hex(&s, g_buffer[i]);
            lab_finish("disc", &s);
        }
    }
    LabLine l = lab_step("lab disc read ");
    lab_put(&l, what);
    lab_put(&l, " ok");
    lab_print(&l);
}

static void read_at(HANDLE file, const char *what, ULONG offset, ULONG length)
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER at;
    at.QuadPart = offset;
    NTSTATUS status = NtReadFile(file, NULL, NULL, NULL, &io, g_buffer, length, &at);
    if (status != STATUS_SUCCESS) fail_status(what, status);
    check_ramp(what, offset, length, &io);
}

static void list(HANDLE dir, const char *pattern_text, const char *label)
{
    ANSI_STRING pattern;
    pattern.Buffer = (PCHAR)pattern_text;
    pattern.Length = (USHORT)lab_strlen(pattern_text);
    pattern.MaximumLength = (USHORT)(pattern.Length + 1);
    ULONG count = 0;
    for (BOOLEAN restart = TRUE;; restart = FALSE) {
        IO_STATUS_BLOCK io;
        FILE_DIRECTORY_INFORMATION *e = (FILE_DIRECTORY_INFORMATION *)g_buffer;
        NTSTATUS status = NtQueryDirectoryFile(dir, NULL, NULL, NULL, &io, g_buffer, sizeof(FILE_DIRECTORY_INFORMATION) + 255,
                                               FileDirectoryInformation, &pattern, restart);
        if (status == STATUS_NO_MORE_FILES) break;
        if (status != STATUS_SUCCESS) fail_status("NtQueryDirectoryFile", status);
        LabLine l = lab_step("lab disc ");
        lab_put(&l, label);
        lab_put(&l, " ");
        e->FileName[e->FileNameLength] = 0;
        lab_put(&l, e->FileName);
        if (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            lab_put(&l, " dir");
        } else {
            lab_put(&l, " file ");
            lab_put_dec(&l, e->EndOfFile.LowPart);
        }
        lab_print(&l);
        count++;
    }
    LabLine l = lab_step("lab disc ");
    lab_put(&l, label);
    lab_put(&l, " count ");
    lab_put_dec(&l, count);
    lab_print(&l);
}

static void missing(const char *path, const char *label)
{
    HANDLE file;
    NTSTATUS status = lab_open(&file, NULL, path, GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    LabLine l = lab_step("lab disc ");
    lab_put(&l, label);
    lab_put(&l, " ");
    lab_put_hex(&l, (ULONG)status);
    lab_print(&l);
    if (status == STATUS_SUCCESS) NtClose(file);
}

void __cdecl lab_entry(void)
{
    DbgPrint("lab disc start");

    HANDLE file;
    NTSTATUS status = lab_open(&file, NULL, "\\??\\D:\\media\\ramp.bin", GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS) fail_status("NtCreateFile D:\\media\\ramp.bin", status);

    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION standard;
    status = NtQueryInformationFile(file, &io, &standard, sizeof standard, FileStandardInformation);
    if (status != STATUS_SUCCESS) fail_status("FileStandardInformation", status);
    LabLine size = lab_step("lab disc ramp.bin size ");
    lab_put_dec(&size, standard.EndOfFile.LowPart);
    lab_print(&size);

    read_at(file, "65530+100", 65530, 100);
    read_at(file, "1000+196608", 1000, 3 * 65536);

    status = NtReadFile(file, NULL, NULL, NULL, &io, g_buffer, 100, NULL);
    if (status != STATUS_SUCCESS) fail_status("NtReadFile at the file pointer", status);
    check_ramp("at the file pointer", 1000 + 3 * 65536, 100, &io);
    FILE_POSITION_INFORMATION position;
    status = NtQueryInformationFile(file, &io, &position, sizeof position, FilePositionInformation);
    if (status != STATUS_SUCCESS) fail_status("FilePositionInformation", status);
    LabLine at = lab_step("lab disc position ");
    lab_put_dec(&at, position.CurrentByteOffset.LowPart);
    lab_print(&at);

    LARGE_INTEGER end;
    end.QuadPart = RAMP_BYTES;
    status = NtReadFile(file, NULL, NULL, NULL, &io, g_buffer, 16, &end);
    LabLine eof = lab_step("lab disc read at the end ");
    lab_put_hex(&eof, (ULONG)status);
    lab_put(&eof, " ");
    lab_put_dec(&eof, io.Information);
    lab_print(&eof);
    NtClose(file);

    HANDLE media;
    status = lab_open(&media, NULL, "\\??\\D:\\media", FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS) fail_status("NtCreateFile D:\\media", status);
    list(media, "*", "entry");
    list(media, "*.txt", "match");

    HANDLE note;
    status = lab_open(&note, media, "deep\\note.txt", GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS) fail_status("NtCreateFile deep\\note.txt relative to D:\\media", status);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    status = NtReadFile(note, NULL, NULL, NULL, &io, g_buffer, 64, &zero);
    if (status != STATUS_SUCCESS) fail_status("NtReadFile deep\\note.txt", status);
    g_buffer[io.Information ? io.Information - 1 : 0] = 0;
    LabLine relative = lab_step("lab disc relative ");
    lab_put(&relative, (const char *)g_buffer);
    lab_print(&relative);
    NtClose(note);
    NtClose(media);

    ANSI_STRING name;
    name.Buffer = "\\??\\D:\\media\\words.txt";
    name.Length = (USHORT)lab_strlen(name.Buffer);
    name.MaximumLength = (USHORT)(name.Length + 1);
    OBJECT_ATTRIBUTES attributes;
    attributes.RootDirectory = NULL;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    FILE_NETWORK_OPEN_INFORMATION full;
    status = NtQueryFullAttributesFile(&attributes, &full);
    if (status != STATUS_SUCCESS) fail_status("NtQueryFullAttributesFile D:\\media\\words.txt", status);
    LabLine words = lab_step("lab disc words.txt size ");
    lab_put_dec(&words, full.EndOfFile.LowPart);
    lab_print(&words);

    missing("\\??\\D:\\media\\missing.bin", "missing file");
    missing("\\??\\D:\\nowhere\\missing.bin", "missing folder");
    lab_finish("disc", NULL);
}
