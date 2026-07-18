/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_HARNESS_H
#define HAX_TESTS_HARNESS_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Temp dirs handed out by t_tempdir(), removed recursively at process
 * exit. Ownership is per-pid: the owner check keeps forked children that
 * never call t_tempdir() from destroying the parent's dirs, and entries
 * below t_tmpdir_first were created by an ancestor (which removes them
 * itself), so a child that does call it cleans only what it created. */
static char **t_tmpdirs;
static size_t t_n_tmpdirs;
static size_t t_tmpdir_first; /* first entry owned by this process */
static pid_t t_tmpdir_owner;

static inline void t_tempdir_cleanup(void)
{
    if (getpid() != t_tmpdir_owner)
        return;
    for (size_t i = 0; i < t_n_tmpdirs; i++) {
        if (i >= t_tmpdir_first) {
            /* Absolute paths: cleanup must not depend on whatever PATH
             * the test exited with. Restore permissions so fixtures
             * locked down to provoke EACCES don't defeat removal — but
             * on directories only (rm needs search/write there; file
             * modes are irrelevant to unlink), so a hard link inside
             * the fixture can't rewrite an external inode's mode.
             * `-exec \;` (not `+`) chmods each dir at visit time,
             * before find descends into it. */
            char cmd[192];
            snprintf(cmd, sizeof(cmd),
                     "/usr/bin/find '%s' -type d -exec /bin/chmod u+rwx {} \\; 2>/dev/null; "
                     "/bin/rm -rf '%s'",
                     t_tmpdirs[i], t_tmpdirs[i]);
            if (system(cmd) != 0)
                fprintf(stderr, "t_tempdir: failed to remove %s\n", t_tmpdirs[i]);
        }
        /* Inherited copies are freed only here, at exit — mid-run they
         * must stay intact, tests hold pointers into them. */
        free(t_tmpdirs[i]);
    }
    free(t_tmpdirs);
    t_tmpdirs = NULL;
    t_n_tmpdirs = 0;
    t_tmpdir_first = 0;
}

/* Create a throwaway directory under /tmp. The harness owns the returned
 * path and removes the whole tree at exit; callers must not free, remove,
 * or outlive-reference it in spawned processes. Aborts on failure: no
 * test proceeds meaningfully without its fixture dir. */
static inline char *t_tempdir(void)
{
    if (t_tmpdir_owner != getpid()) {
        /* First call in this process. In a fork child the inherited
         * entries stay intact (the parent removes those dirs); everything
         * from here on is this process's to remove. Registering the
         * handler again is harmless: handlers run LIFO, so the inherited
         * registration sees an already-emptied list. */
        t_tmpdir_first = t_n_tmpdirs;
        t_tmpdir_owner = getpid();
        atexit(t_tempdir_cleanup);
    }
    char *dir = strdup("/tmp/hax_test_XXXXXX");
    if (!dir || !mkdtemp(dir)) {
        fprintf(stderr, "t_tempdir: %s\n", strerror(errno));
        abort();
    }
    char **grown = realloc(t_tmpdirs, (t_n_tmpdirs + 1) * sizeof(*grown));
    if (!grown)
        abort();
    t_tmpdirs = grown;
    t_tmpdirs[t_n_tmpdirs++] = dir;
    return dir;
}

#define T_REPORT()                                                                                 \
    do {                                                                                           \
        if (t_skips)                                                                               \
            fprintf(stderr, "%d skipped\n", t_skips);                                              \
        if (t_failures)                                                                            \
            fprintf(stderr, "%d failures\n", t_failures);                                          \
        return t_failures != 0;                                                                    \
    } while (0)

#endif /* HAX_TESTS_HARNESS_H */
