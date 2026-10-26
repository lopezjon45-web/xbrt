/* Shadows the CRT's <assert.h> for the xemu side (the /I directory comes before the system ones). Upstream's asserts
 * stay on and each failure is a named refusal with the expression, file and line, instead of the CRT's message box in
 * a windows-subsystem app. No include guard: <assert.h> is re-includable by contract. */
#undef assert
__declspec(noreturn) void xbrt_assert_failed(const char *expr, const char *file, int line);
#define assert(e) ((e) ? (void)0 : xbrt_assert_failed(#e, __FILE__, __LINE__))
#ifndef static_assert
#define static_assert _Static_assert
#endif
