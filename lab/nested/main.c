#include "../lab.h"

typedef struct {
    ULONG depth, count;
    ULONG order[6];
    ULONG esp[3];
} Nest;

static KINTERRUPT synchronize_object;
static Nest nest;

static BOOLEAN NTAPI never_connected(PKINTERRUPT i, PVOID context) { return FALSE; }

static BOOLEAN NTAPI level(PVOID context)
{
    Nest *n = context;
    ULONG d = ++n->depth;
    n->order[n->count++] = d;
    __asm__ volatile("movl %%esp, %0" : "=r"(n->esp[d - 1]));
    BOOLEAN inner = TRUE;
    if (d < 3) {
        inner = KeSynchronizeExecution(&synchronize_object, level, context);
    } else {
        LARGE_INTEGER t;
        KeQuerySystemTime(&t);
    }
    n->order[n->count++] = 10 + d;
    n->depth--;
    return inner;
}

void __cdecl lab_entry(void)
{
    DbgPrint("lab nested start");
    KeInitializeInterrupt(&synchronize_object, never_connected, NULL, 0x3F, 15, LevelSensitive, FALSE);
    BOOLEAN result = KeSynchronizeExecution(&synchronize_object, level, &nest);

    LabLine order = lab_step("lab nested order");
    for (ULONG i = 0; i < nest.count && i < 6; i++) {
        lab_put(&order, " ");
        lab_put_dec(&order, nest.order[i]);
    }
    lab_print(&order);

    if (result != TRUE) {
        LabLine s = lab_step("KeSynchronizeExecution returned ");
        lab_put_hex(&s, result);
        lab_finish("nested", &s);
    }
    if (nest.count != 6 || !(nest.esp[0] > nest.esp[1] && nest.esp[1] > nest.esp[2])) {
        LabLine s = lab_step("levels did not nest on one stack: esp ");
        for (int i = 0; i < 3; i++) {
            lab_put_hex(&s, nest.esp[i]);
            lab_put(&s, " ");
        }
        lab_finish("nested", &s);
    }
    lab_finish("nested", NULL);
}
