#ifndef QEMU_TIMER_H
#define QEMU_TIMER_H

#include "qemu/osdep.h"

typedef enum QEMUClockType { QEMU_CLOCK_REALTIME, QEMU_CLOCK_VIRTUAL, QEMU_CLOCK_HOST, QEMU_CLOCK_MAX } QEMUClockType;
#define NANOSECONDS_PER_SECOND 1000000000LL
#define SCALE_MS 1000000
#define SCALE_US 1000
#define SCALE_NS 1

typedef void QEMUTimerCB(void *opaque);

struct QEMUTimer {
    int64_t expire_time;
    QEMUClockType type;
    QEMUTimerCB *cb;
    void *opaque;
    QEMUTimer *next;
    bool pending;
};

int64_t qemu_clock_get_ns(QEMUClockType type);
static inline int64_t qemu_clock_get_us(QEMUClockType type) { return qemu_clock_get_ns(type) / SCALE_US; }
static inline int64_t qemu_clock_get_ms(QEMUClockType type) { return qemu_clock_get_ns(type) / SCALE_MS; }

void timer_init_ns(QEMUTimer *ts, QEMUClockType type, QEMUTimerCB *cb, void *opaque);
void timer_mod(QEMUTimer *ts, int64_t expire_time);
void timer_del(QEMUTimer *ts);
bool timer_pending(QEMUTimer *ts);

#endif
