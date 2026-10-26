#ifndef XBRT_SHIM_GLIB_H
#define XBRT_SHIM_GLIB_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef char gchar;
typedef int gint;
typedef unsigned int guint;
typedef int gboolean;
typedef void *gpointer;
typedef size_t gsize;

typedef struct GString {
    gchar *str;
    gsize len;
    gsize allocated_len;
} GString;

void *g_malloc(size_t n);
void *g_malloc0(size_t n);
void *g_malloc0_n(size_t count, size_t size);
void *g_realloc(void *p, size_t n);
void g_free(void *p);
gchar *g_strdup(const gchar *s);
gchar *g_strdup_printf(const gchar *fmt, ...) __attribute__((format(printf, 1, 2)));
gchar *g_strdup_vprintf(const gchar *fmt, va_list args);
#define g_new(type, n) ((type *)g_malloc0_n((n), sizeof(type)))
#define g_new0(type, n) ((type *)g_malloc0_n((n), sizeof(type)))

GString *g_string_new(const gchar *init);
GString *g_string_new_take(gchar *init);
gchar *g_string_free(GString *s, gboolean free_segment);
GString *g_string_append(GString *s, const gchar *text);
void g_string_append_vprintf(GString *s, const gchar *fmt, va_list args);
void g_string_append_printf(GString *s, const gchar *fmt, ...) __attribute__((format(printf, 2, 3)));

static inline void xbrt_g_autofree_cleanup(void *p) { g_free(*(void **)p); }
#define g_autofree __attribute__((cleanup(xbrt_g_autofree_cleanup)))
#define G_DEFINE_AUTOPTR_CLEANUP_FUNC(type, fn)                                                                        \
    static inline void xbrt_autoptr_cleanup_##type(type **p)                                                           \
    {                                                                                                                  \
        if (*p) fn(*p);                                                                                                \
    }
#define g_autoptr(type) __attribute__((cleanup(xbrt_autoptr_cleanup_##type))) type *

#endif
