#ifndef QEMU_ERROR_REPORT_H
#define QEMU_ERROR_REPORT_H

#include "qemu/osdep.h"

void error_report(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void warn_report(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

#endif
