/* The checks of the user-space test programs (utest, usbtest). A test is
 * a bool function; a check that fails prints
 *     <CHECK_PROG>: <CHECK_CUR>: FAILED at line <n>: <what>
 * and returns false from it. Like the kernel's KT_* macros these hide a
 * return: that is the point of them, and they are only for test bodies.
 *
 * Before including this, define CHECK_PROG (the program's name, a string
 * literal) and CHECK_CUR (an expression: the name of the test running). */
#pragma once

#include <os.h>

#define FAIL(...)                                                           \
    do {                                                                    \
        printf(CHECK_PROG ": %s: FAILED at line %d: ", CHECK_CUR, __LINE__); \
        printf(__VA_ARGS__);                                                \
        printf("\n");                                                       \
        return false;                                                       \
    } while (0)
#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c))                                                           \
            FAIL("%s", #c);                                                 \
    } while (0)
#define CHECK_ST(expr, want)                                                \
    do {                                                                    \
        status_t _s = (expr), _w = (want);                                  \
        if (_s != _w)                                                       \
            FAIL("%s is %s, want %s", #expr, status_str(_s), status_str(_w)); \
    } while (0)
#define CHECK_EQ(a, b)                                                      \
    do {                                                                    \
        int64_t _a = (int64_t)(a), _b = (int64_t)(b);                       \
        if (_a != _b)                                                       \
            FAIL("%s == %s: %ld vs %ld", #a, #b, (long)_a, (long)_b);       \
    } while (0)
