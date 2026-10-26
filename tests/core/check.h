#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char *name;
    std::function<void()> body;
};

inline std::vector<Test> &tests()
{
    static std::vector<Test> t;
    return t;
}

inline int &failures()
{
    static int n = 0;
    return n;
}

struct Register {
    Register(const char *name, std::function<void()> body) { tests().push_back({name, std::move(body)}); }
};

inline void fail(const char *file, int line, const char *expr, const std::string &detail)
{
    failures()++;
    std::printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, detail.empty() ? "" : "  ", detail.c_str());
}

inline int run_all()
{
    for (const Test &t : tests()) {
        std::printf("test %s\n", t.name);
        t.body();
    }
    std::printf("%zu tests, %d failures\n", tests().size(), failures());
    return failures() ? 1 : 0;
}

}  // namespace check

#define TEST(name) \
    static void test_##name(); \
    static check::Register reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(expr) \
    do { \
        if (!(expr)) check::fail(__FILE__, __LINE__, #expr, ""); \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        auto va_ = (a); \
        auto vb_ = (b); \
        if (!(va_ == vb_)) check::fail(__FILE__, __LINE__, #a " == " #b, "got " + std::to_string(va_) + " and " + std::to_string(vb_)); \
    } while (0)

#define CHECK_STR(a, b) \
    do { \
        std::string va_ = (a); \
        std::string vb_ = (b); \
        if (va_ != vb_) check::fail(__FILE__, __LINE__, #a " == " #b, "got \"" + va_ + "\" and \"" + vb_ + "\""); \
    } while (0)

#define CHECK_CONTAINS(text, part) \
    do { \
        std::string vt_ = (text); \
        std::string vp_ = (part); \
        if (vt_.find(vp_) == std::string::npos) check::fail(__FILE__, __LINE__, #text " contains " #part, "got \"" + vt_ + "\""); \
    } while (0)
