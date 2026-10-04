/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_HARNESS_H
#define HAX_TESTS_HARNESS_H

#include <stdio.h>
#include <string.h>

/* Sanitizer detection, for tests that must widen or skip timing-sensitive checks that sanitizer
 * interceptors (notably fork) slow by orders of magnitude. Clang reports via __has_feature, gcc via
 * __SANITIZE_*__. */
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

/* This process's tallies; T_REPORT turns them into the exit status. A forked child inherits its
 * parent's counts. */
extern int t_failures;
extern int t_skips;

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

/* Bail out of the current (void) test function, recording why. On passing runs the note lands in
 * the captured test log, not the console. */
#define T_SKIP(why)                                                                                \
    do {                                                                                           \
        fprintf(stderr, "%s:%d: skip: %s\n", __FILE__, __LINE__, why);                             \
        t_skips++;                                                                                 \
        return;                                                                                    \
    } while (0)

/* End main(), exiting nonzero if anything failed. */
#define T_REPORT()                                                                                 \
    do {                                                                                           \
        if (t_skips)                                                                               \
            fprintf(stderr, "%d skipped\n", t_skips);                                              \
        if (t_failures)                                                                            \
            fprintf(stderr, "%d failures\n", t_failures);                                          \
        return t_failures != 0;                                                                    \
    } while (0)

/* Create a scratch directory under /tmp and return its canonical path: on macOS, /tmp is a symlink
 * that getcwd() resolves. The harness owns the path and removes the tree when the creating process
 * exits, even if a fixture locked it down; callers must not free or remove it, nor leave spawned
 * processes using it. Aborts on failure. */
char *t_tempdir(void);

/* Replace PATH with `value` (NULL unsets it) and return the previous value for t_path_restore, NULL
 * when it was unset. Aborts on allocation failure. */
char *t_path_replace(const char *value);

/* Put `dir` ahead of the current PATH entries, so a stub there shadows the real command while the
 * utilities the stub runs stay reachable. Returns the previous value for t_path_restore. */
char *t_path_prepend(const char *dir);

/* Restore PATH from t_path_replace or t_path_prepend and release the saved copy. */
void t_path_restore(char *saved);

#endif /* HAX_TESTS_HARNESS_H */
