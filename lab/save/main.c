#include "../lab.h"

// Saves as XAPI makes them: T: and U: linked to the title's TDATA\<id> and UDATA\<id> folders on the hard disk, a
// file written, closed, reopened and read back on each. T:\lab\state.bin counts the launches, so a second launch with
// the same saves reads the first one's count. The title id is the image's own, from its certificate.

#define XBE_BASE 0x00010000u
#define XBE_CERTIFICATE_ADDRESS 0x118u
#define CERTIFICATE_TITLE_ID 0x8u

static void fail_status(const char *what, NTSTATUS status)
{
    LabLine s = lab_step_status(what, status);
    lab_finish("save", &s);
}

static void put_id(char *out, ULONG id)
{
    for (int i = 0; i < 8; i++) out[i] = "0123456789ABCDEF"[(id >> (28 - 4 * i)) & 0xF];
}

static void link_drive(char letter, const char *folder, ULONG id)
{
    char target[64];
    unsigned n = 0;
    for (const char *p = "\\Device\\Harddisk0\\Partition1\\"; *p;) target[n++] = *p++;
    for (const char *f = folder; *f;) target[n++] = *f++;
    target[n++] = '\\';
    put_id(target + n, id);
    target[n + 8] = 0;

    HANDLE dir;
    NTSTATUS status = lab_open(&dir, NULL, target, FILE_LIST_DIRECTORY, FILE_OPEN_IF, FILE_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS) fail_status(target, status);
    NtClose(dir);

    char link_text[7] = {'\\', '?', '?', '\\', letter, ':', 0};
    ANSI_STRING link, device;
    link.Buffer = link_text;
    link.Length = (USHORT)lab_strlen(link_text);
    link.MaximumLength = (USHORT)(link.Length + 1);
    device.Buffer = target;
    device.Length = (USHORT)lab_strlen(target);
    device.MaximumLength = (USHORT)(device.Length + 1);
    status = IoCreateSymbolicLink(&link, &device);
    if (status != STATUS_SUCCESS) fail_status("IoCreateSymbolicLink", status);
}

static void make_folder(const char *path)
{
    HANDLE dir;
    NTSTATUS status = lab_open(&dir, NULL, path, FILE_LIST_DIRECTORY, FILE_OPEN_IF, FILE_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS) fail_status(path, status);
    NtClose(dir);
}

static void write_whole(const char *path, ULONG disposition, const void *data, ULONG length)
{
    HANDLE file;
    NTSTATUS status = lab_open(&file, NULL, path, GENERIC_READ | GENERIC_WRITE, disposition, FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS) fail_status(path, status);
    IO_STATUS_BLOCK io;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    status = NtWriteFile(file, NULL, NULL, NULL, &io, (PVOID)data, length, &zero);
    if (status != STATUS_SUCCESS || io.Information != length) fail_status("NtWriteFile", status);
    NtClose(file);
}

// Bytes read from the start of `path`, opened as an existing file.
static ULONG read_back(const char *path, void *out, ULONG length)
{
    HANDLE file;
    NTSTATUS status = lab_open(&file, NULL, path, GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS) fail_status(path, status);
    IO_STATUS_BLOCK io;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    status = NtReadFile(file, NULL, NULL, NULL, &io, out, length, &zero);
    NtClose(file);
    if (status == STATUS_END_OF_FILE) return 0;
    if (status != STATUS_SUCCESS) fail_status("NtReadFile", status);
    return io.Information;
}

void __cdecl lab_entry(void)
{
    DbgPrint("lab save start");
    ULONG certificate = *(volatile ULONG *)(XBE_BASE + XBE_CERTIFICATE_ADDRESS);
    ULONG id = *(volatile ULONG *)(certificate + CERTIFICATE_TITLE_ID);
    link_drive('T', "TDATA", id);
    link_drive('U', "UDATA", id);

    make_folder("\\??\\T:\\lab");
    ULONG launches = 0;
    HANDLE state;
    NTSTATUS status = lab_open(&state, NULL, "\\??\\T:\\lab\\state.bin", GENERIC_READ | GENERIC_WRITE, FILE_OPEN_IF,
                               FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS) fail_status("T:\\lab\\state.bin", status);
    NtClose(state);
    read_back("\\??\\T:\\lab\\state.bin", &launches, sizeof launches);
    launches++;
    write_whole("\\??\\T:\\lab\\state.bin", FILE_OPEN_IF, &launches, sizeof launches);
    ULONG again = 0;
    if (read_back("\\??\\T:\\lab\\state.bin", &again, sizeof again) != sizeof again || again != launches) {
        LabLine s = lab_step("T:\\lab\\state.bin read back ");
        lab_put_dec(&s, again);
        lab_finish("save", &s);
    }
    LabLine t = lab_step("lab save title data launch ");
    lab_put_dec(&t, launches);
    lab_print(&t);

    make_folder("\\??\\U:\\slot");
    char text[7] = {'s', 'a', 'v', 'e', ' ', (char)('0' + launches % 10), 0};
    write_whole("\\??\\U:\\slot\\save.dat", FILE_OVERWRITE_IF, text, sizeof text);
    char got[16];
    ULONG n = read_back("\\??\\U:\\slot\\save.dat", got, sizeof got);
    for (ULONG i = 0; i < sizeof text; i++) {
        if (n != sizeof text || got[i] != text[i]) {
            LabLine s = lab_step("U:\\slot\\save.dat read back ");
            lab_put_dec(&s, n);
            lab_put(&s, " bytes");
            lab_finish("save", &s);
        }
    }
    DbgPrint("lab save user data ok");
    lab_finish("save", NULL);
}
