#include "../lab.h"

// Both USB host controllers answer at their apertures: OHCI revision 1.0 and a root hub with ports.

static ULONG read_register(ULONG address) { return *(volatile ULONG *)address; }

void __cdecl lab_entry(void)
{
    DbgPrint("lab usb start");
    static const ULONG controllers[2] = {0xFED00000, 0xFED08000};
    for (int i = 0; i < 2; i++) {
        ULONG revision = read_register(controllers[i]) & 0xFF;
        ULONG ports = read_register(controllers[i] + 0x48) & 0xFF;
        LabLine l = lab_step("lab usb controller ");
        lab_put_dec(&l, i);
        lab_put(&l, " revision ");
        lab_put_hex(&l, revision);
        lab_print(&l);
        if (revision != 0x10 || ports == 0) {
            LabLine s = lab_step("controller ");
            lab_put_dec(&s, i);
            lab_put(&s, " revision ");
            lab_put_hex(&s, revision);
            lab_put(&s, " ports ");
            lab_put_dec(&s, ports);
            lab_finish("usb", &s);
        }
    }
    lab_finish("usb", NULL);
}
