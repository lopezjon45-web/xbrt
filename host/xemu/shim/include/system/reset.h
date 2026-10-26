#ifndef SYSTEM_RESET_H
#define SYSTEM_RESET_H

void qemu_register_reset(void (*fn)(void *opaque), void *opaque);

#endif
