#include "shim.h"

struct Error {
    char *msg;
};

Error *error_fatal;

void error_setg_internal(Error **errp, const char *file, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char *msg = g_strdup_vprintf(fmt, args);
    va_end(args);
    if (errp == &error_fatal) xbrt_refuse(XBRT_REFUSE_FEATURE, "xemu fatal error at %s:%d: %s", file, line, msg);
    if (!errp) {
        g_free(msg);
        return;
    }
    Error *err = g_malloc0(sizeof *err);
    err->msg = msg;
    *errp = err;
}

void error_propagate(Error **dst, Error *local)
{
    if (!local) return;
    if (dst == &error_fatal) xbrt_refuse(XBRT_REFUSE_FEATURE, "xemu fatal error: %s", local->msg);
    if (dst && !*dst) *dst = local;
    else error_free(local);
}

const char *error_get_pretty(const Error *err) { return err->msg; }

void error_free(Error *err)
{
    if (!err) return;
    g_free(err->msg);
    g_free(err);
}

static void log_line(const char *prefix, const char *msg)
{
    char *line = g_strdup_printf("xemu: %s%s", prefix, msg);
    g_xbrt_host->log(line);
    g_free(line);
}

void error_report_err(Error *err)
{
    log_line("error: ", err->msg);
    error_free(err);
}

void warn_reportf_err(Error *err, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char *prefix = g_strdup_vprintf(fmt, args);
    va_end(args);
    log_line(prefix, err->msg);
    g_free(prefix);
    error_free(err);
}

void error_report(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char *msg = g_strdup_vprintf(fmt, args);
    va_end(args);
    log_line("error: ", msg);
    g_free(msg);
}

void warn_report(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char *msg = g_strdup_vprintf(fmt, args);
    va_end(args);
    log_line("warning: ", msg);
    g_free(msg);
}
