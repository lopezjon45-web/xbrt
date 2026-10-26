#ifndef QEMU_MAIN_LOOP_H
#define QEMU_MAIN_LOOP_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "system/runstate.h"

void bql_lock_impl(const char *file, int line);
void bql_unlock(void);
bool bql_locked(void);
#define bql_lock() bql_lock_impl(__FILE__, __LINE__)

#endif
