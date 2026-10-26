/* An allocation failure is a refusal, as glib aborts. */
#include "shim.h"

static void *checked(void *p, size_t n)
{
    if (!p && n) xbrt_refuse(XBRT_REFUSE_FEATURE, "out of memory allocating %zu bytes", n);
    return p;
}

void *g_malloc(size_t n) { return checked(malloc(n ? n : 1), n); }
void *g_malloc0(size_t n) { return checked(calloc(1, n ? n : 1), n); }

void *g_malloc0_n(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) xbrt_refuse(XBRT_REFUSE_FEATURE, "g_malloc0_n(%zu, %zu) overflows", count, size);
    return g_malloc0(count * size);
}

void *g_realloc(void *p, size_t n) { return checked(realloc(p, n ? n : 1), n); }
void g_free(void *p) { free(p); }

gchar *g_strdup(const gchar *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    return memcpy(g_malloc(n), s, n);
}

gchar *g_strdup_vprintf(const gchar *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    gchar *s = g_malloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, args);
    return s;
}

gchar *g_strdup_printf(const gchar *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    gchar *s = g_strdup_vprintf(fmt, args);
    va_end(args);
    return s;
}

static void reserve(GString *s, gsize extra)
{
    if (s->len + extra + 1 <= s->allocated_len) return;
    gsize want = MAX(s->allocated_len * 2, s->len + extra + 1);
    s->str = g_realloc(s->str, want);
    s->allocated_len = want;
}

GString *g_string_new(const gchar *init)
{
    GString *s = g_malloc0(sizeof *s);
    reserve(s, init ? strlen(init) : 0);
    s->str[0] = 0;
    if (init) g_string_append(s, init);
    return s;
}

GString *g_string_new_take(gchar *init)
{
    GString *s = g_string_new(init);
    g_free(init);
    return s;
}

gchar *g_string_free(GString *s, gboolean free_segment)
{
    gchar *str = s->str;
    g_free(s);
    if (free_segment) {
        g_free(str);
        return NULL;
    }
    return str;
}

GString *g_string_append(GString *s, const gchar *text)
{
    gsize n = strlen(text);
    reserve(s, n);
    memcpy(s->str + s->len, text, n + 1);
    s->len += n;
    return s;
}

void g_string_append_vprintf(GString *s, const gchar *fmt, va_list args)
{
    gchar *text = g_strdup_vprintf(fmt, args);
    g_string_append(s, text);
    g_free(text);
}

void g_string_append_printf(GString *s, const gchar *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    g_string_append_vprintf(s, fmt, args);
    va_end(args);
}
