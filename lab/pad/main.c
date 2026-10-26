// A pad as a title's own USB code finds it: nxdk's OHCI host driver (interrupt-driven, through the kernel's
// KeConnectInterrupt and DPCs) and its XID driver enumerate the controller on the first USB controller, and the lab
// prints each change of its reports. The desktop host's scripted pad holds A with the left stick right for half of
// every two seconds; on the console a person presses A and pushes the stick. Built by nxdk's own Makefile (crt0 and
// main), not lab/build.sh: the USB stack needs nxdk's runtime.
#include <usbh_lib.h>
#include <xid_driver.h>
#include <windows.h>

#include "../lab.h"

#define WAIT_SECONDS 20
#define PRESSED 0x40
#define STICK_PUSHED 16000

static xid_dev_t *volatile g_pad;
static xid_gamepad_in g_report;
static volatile LONG g_reports;

static void on_connect(xid_dev_t *xid, int status)
{
    (void)status;
    if (usbh_xid_get_type(xid) != XREMOTE && usbh_xid_get_type(xid) != STEEL_BATTALION) g_pad = xid;
}

static void on_disconnect(xid_dev_t *xid, int status)
{
    (void)status;
    if (g_pad == xid) g_pad = NULL;
}

// The XID driver's completion, at DISPATCH_LEVEL: keep the report; the main loop reads it.
static void on_report(UTR_T *utr)
{
    if (utr->status != 0 || utr->xfer_len < sizeof(xid_gamepad_in)) return;
    for (unsigned i = 0; i < sizeof(xid_gamepad_in); i++) ((uint8_t *)&g_report)[i] = utr->buff[i];
    InterlockedIncrement(&g_reports);
}

// The judged line says what was seen; the values follow on a line of their own for a reader of the log.
static void print_state(const char *what, const xid_gamepad_in *r)
{
    LabLine l = lab_step("lab pad ");
    lab_put(&l, what);
    lab_print(&l);
    LabLine v = lab_step("pad report a ");
    lab_put_dec(&v, r->a);
    lab_put(&v, " left stick x ");
    if (r->leftStickX < 0) lab_put(&v, "-");
    lab_put_dec(&v, (ULONG)(r->leftStickX < 0 ? -r->leftStickX : r->leftStickX));
    lab_print(&v);
}

int main(void)
{
    DbgPrint("lab pad start");
    usbh_core_init();
    usbh_xid_init();
    usbh_install_xid_conn_callback(on_connect, on_disconnect);

    BOOL announced = FALSE, saw_down = FALSE, saw_up = FALSE;
    LONG seen = 0;
    for (int tick = 0; tick < WAIT_SECONDS * 100 && !saw_up; tick++) {
        usbh_pooling_hubs();
        xid_dev_t *pad = g_pad;
        if (pad && !announced) {
            DbgPrint("lab pad connected");
            announced = TRUE;
        }
        if (pad) usbh_xid_read(pad, 0, on_report);
        if (g_reports != seen) {
            seen = g_reports;
            xid_gamepad_in r = g_report;
            BOOL down = r.a >= PRESSED && r.leftStickX >= STICK_PUSHED;
            if (down && !saw_down) {
                print_state("pressed A with the left stick right", &r);
                saw_down = TRUE;
            } else if (saw_down && r.a < PRESSED && r.leftStickX < STICK_PUSHED) {
                print_state("released both", &r);
                saw_up = TRUE;
            }
        }
        Sleep(10);
    }
    if (!saw_up) {
        LabLine s = lab_step(!announced ? "no pad connected" : !saw_down ? "A never pressed with the stick right" : "A never released");
        lab_put(&s, " in ");
        lab_put_dec(&s, WAIT_SECONDS);
        lab_put(&s, " s, ");
        lab_put_dec(&s, (ULONG)g_reports);
        lab_put(&s, " reports");
        lab_finish("pad", &s);
    }
    lab_finish("pad", NULL);
    return 0;
}
