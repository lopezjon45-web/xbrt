#ifndef QAPI_ERROR_H
#define QAPI_ERROR_H

#include "qemu/osdep.h"

extern Error *error_fatal;
void error_setg_internal(Error **errp, const char *file, int line, const char *fmt, ...) G_GNUC_PRINTF(4, 5);
#define error_setg(errp, ...) error_setg_internal((errp), __FILE__, __LINE__, __VA_ARGS__)
void error_propagate(Error **dst, Error *local);
const char *error_get_pretty(const Error *err);
void error_free(Error *err);
void error_report_err(Error *err);
void warn_reportf_err(Error *err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

#endif
