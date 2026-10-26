#ifndef SYSTEM_RUNSTATE_H
#define SYSTEM_RUNSTATE_H

#include "qemu/osdep.h"

typedef enum RunState {
    RUN_STATE_RUNNING,
    RUN_STATE_PAUSED,
    RUN_STATE_RESTORE_VM,
    RUN_STATE_SAVE_VM,
    RUN_STATE_SHUTDOWN,
} RunState;

typedef void VMChangeStateHandler(void *opaque, bool running, RunState state);
VMChangeStateEntry *qemu_add_vm_change_state_handler(VMChangeStateHandler *cb, void *opaque);

#endif
