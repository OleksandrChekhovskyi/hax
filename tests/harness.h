/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_HARNESS_H
#define HAX_TESTS_HARNESS_H

#include <stdio.h>
#include <string.h>

/* Sanitizer detection, for tests that must widen or skip timing-sensitive
 * checks that sanitizer interceptors (notably fork) slow by orders of
 * magnitude. Clang reports via __has_feature, gcc via __SANITIZE_*__. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define T_TSAN 1
#endif
#if __has_feature(address_sanitizer)
#define T_ASAN 1
#endif
#endif
#if !defined(T_TSAN) && defined(__SANITIZE_THREAD__)
#define T_TSAN 1
#endif
#if !defined(T_ASAN) && defined(__SANITIZE_ADDRESS__)
#define T_ASAN 1
#endif

/* Each test binary is a single translation unit, so a `static` counter
 * here gives us exactly one per binary with no link-time coordination. */
static int t_failures = 0;
static int t_skips = 0;

#define FAIL(fmt, ...)                                                                             \
    do {                                                                                           \
        fprintf(stderr, "%s:%d: error: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);               \
        t_failures++;                                                                              \
    } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond))                                                                               \
            FAIL("%s", #cond);                                                                     \
    } while (0)

#define EXPECT_STR_EQ(got, want)                                                                   \
    do {                                                                                           \
        const char *_g = (got), *_w = (want);                                                      \
        if (strcmp(_g, _w) != 0)                                                                   \
            FAIL("want \"%s\", got \"%s\"", _w, _g);                                               \
    } while (0)

#define EXPECT_MEM_EQ(got, got_len, want, want_len)                                                \
    do {                                                                                           \
        size_t _gl = (got_len), _wl = (want_len);                                                  \
        if (_gl != _wl || memcmp((got), (want), _wl) != 0)                                         \
            FAIL("bytes mismatch: want %zu, got %zu", _wl, _gl);                                   \
    } while (0)

/* Bail out of the current (void) test function, recording why. The note
 * lands in the captured test log, not the console, on passing runs. */
#define T_SKIP(why)                                                                                \
    do {                                                                                           \
        fprintf(stderr, "%s:%d: skip: %s\n", __FILE__, __LINE__, why);                             \
        t_skips++;                                                                                 \
        return;                                                                                    \
    } while (0)

#define T_REPORT()                                                                                 \
    do {                                                                                           \
        if (t_skips)                                                                               \
            fprintf(stderr, "%d skipped\n", t_skips);                                              \
        if (t_failures)                                                                            \
            fprintf(stderr, "%d failures\n", t_failures);                                          \
        return t_failures != 0;                                                                    \
    } while (0)

#endif /* HAX_TESTS_HARNESS_H */
