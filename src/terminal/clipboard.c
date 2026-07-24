/* SPDX-License-Identifier: MIT */
#include "terminal/clipboard.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "util.h"
#include "system/path.h"
#include "system/spawn.h"
#include "text/base64.h"

/* Read-side caps. A clipboard image is a screenshot or a copied picture
 * — tens of MB at the very most; a runaway helper (or a copied 4GB
 * file posing as image/png) shouldn't OOM the REPL. Text matches the
 * editor's own bracketed-paste insert cap (1 MiB). */
#define CLIPBOARD_IMAGE_MAX_BYTES (64u << 20)
#define CLIPBOARD_TEXT_MAX_BYTES  (1u << 20)

char *clipboard_osc52_sequence(const char *text, size_t len, int tmux_wrap, size_t *out_len)
{
    if (len > CLIPBOARD_OSC52_MAX_BYTES)
        return NULL;

    size_t b64_len = 0;
    char *b64 = base64_encode(text, len, &b64_len);

    /* Inner OSC 52 form: ESC ] 5 2 ; c ; <b64> BEL  (7 fixed bytes + b64).
     * tmux passthrough wraps it as ESC P t m u x ; <inner-with-doubled-ESC> ESC \,
     * and the inner form contains exactly one ESC, so doubling it adds one byte. */
    char *out;
    size_t out_n;
    if (tmux_wrap) {
        out_n = 7 /* ESC P t m u x ; */ + 1 /* extra ESC for the doubled inner ESC */
                + 7 /* ESC ] 5 2 ; c ; */ + b64_len + 1 /* BEL */ + 2 /* ESC \ */;
        out = xmalloc(out_n + 1);
        size_t p = 0;
        out[p++] = 0x1b;
        out[p++] = 'P';
        out[p++] = 't';
        out[p++] = 'm';
        out[p++] = 'u';
        out[p++] = 'x';
        out[p++] = ';';
        out[p++] = 0x1b; /* doubled ESC of the inner sequence */
        out[p++] = 0x1b;
        out[p++] = ']';
        out[p++] = '5';
        out[p++] = '2';
        out[p++] = ';';
        out[p++] = 'c';
        out[p++] = ';';
        memcpy(out + p, b64, b64_len);
        p += b64_len;
        out[p++] = 0x07;
        out[p++] = 0x1b;
        out[p++] = '\\';
        out[p] = '\0';
    } else {
        out_n = 7 + b64_len + 1;
        out = xmalloc(out_n + 1);
        size_t p = 0;
        out[p++] = 0x1b;
        out[p++] = ']';
        out[p++] = '5';
        out[p++] = '2';
        out[p++] = ';';
        out[p++] = 'c';
        out[p++] = ';';
        memcpy(out + p, b64, b64_len);
        p += b64_len;
        out[p++] = 0x07;
        out[p] = '\0';
    }
    free(b64);
    if (out_len)
        *out_len = out_n;
    return out;
}

/* fork+exec a clipboard helper, piping `text` to its stdin. The child's
 * stdout/stderr are redirected to /dev/null so a noisy helper can't
 * scribble on the REPL (and "command not found" diagnostics from
 * execvp failure stay invisible to the user). Signal etiquette during
 * the child run is delegated to spawn.c's helpers — same parent/child
 * mask as spawn_run, the only differences here are argv-based exec
 * (so we can probe PATH via execvp's ENOENT) and the /dev/null fd
 * redirection. Returns 0 iff the child exited 0 (which also implies
 * execvp found it on PATH); -1 covers everything else. */
static int spawn_pipe_in(const char *cmd, const char *const *argv, const char *text, size_t len)
{
    int p[2];
    if (pipe(p) < 0)
        return -1;

    struct sigaction saved_int, saved_quit, saved_pipe;
    spawn_parent_ignore(&saved_int, &saved_quit, &saved_pipe);

    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        spawn_parent_restore(&saved_int, &saved_quit, &saved_pipe);
        return -1;
    }
    if (pid == 0) {
        close(p[1]);
        if (dup2(p[0], STDIN_FILENO) < 0)
            _exit(127);
        close(p[0]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > 2)
                close(dn);
        }
        spawn_child_default_signals();
        execvp(cmd, (char *const *)argv);
        _exit(127);
    }
    close(p[0]);
    int wr = write_all(p[1], text, len);
    close(p[1]);
    int status = spawn_wait_child(pid);
    spawn_parent_restore(&saved_int, &saved_quit, &saved_pipe);
    if (status < 0 || wr < 0)
        return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    return -1;
}

static int try_native(const char *text, size_t len)
{
    /* macOS: pbcopy is the only game in town. On Linux this exec just
     * fails with ENOENT and we fall through — cheaper than gating on
     * uname or compile-time platform macros. */
    {
        const char *argv[] = {"pbcopy", NULL};
        if (spawn_pipe_in("pbcopy", argv, text, len) == 0)
            return 0;
    }
    /* Wayland: try wl-copy first when WAYLAND_DISPLAY is set. xclip /
     * xsel may "work" via XWayland but write to a clipboard the user
     * can't paste from in Wayland-native apps, so order matters. We
     * only try wl-copy when WAYLAND_DISPLAY is set — without it the
     * tool can't connect to a compositor and would always fail. */
    if (getenv("WAYLAND_DISPLAY")) {
        const char *argv[] = {"wl-copy", NULL};
        if (spawn_pipe_in("wl-copy", argv, text, len) == 0)
            return 0;
    }
    {
        const char *argv[] = {"xclip", "-selection", "clipboard", NULL};
        if (spawn_pipe_in("xclip", argv, text, len) == 0)
            return 0;
    }
    {
        const char *argv[] = {"xsel", "-b", "-i", NULL};
        if (spawn_pipe_in("xsel", argv, text, len) == 0)
            return 0;
    }
    return -1;
}

static int write_osc52(const char *text, size_t len)
{
    int tmux_wrap = (getenv("TMUX") != NULL);
    size_t seq_len = 0;
    char *seq = clipboard_osc52_sequence(text, len, tmux_wrap, &seq_len);
    if (!seq)
        return -1;

    /* Prefer /dev/tty so a redirected stdout can't swallow the escape
     * sequence (and so the user doesn't see a wall of base64 if stdout
     * isn't a TTY for some reason). */
    int fd = open("/dev/tty", O_WRONLY | O_NOCTTY);
    int opened = (fd >= 0);
    if (!opened)
        fd = STDOUT_FILENO;
    int rc = write_all(fd, seq, seq_len);
    if (opened)
        close(fd);
    free(seq);
    return rc;
}

static int is_ssh(void)
{
    return getenv("SSH_TTY") != NULL || getenv("SSH_CONNECTION") != NULL;
}

/* Read-direction capture under the shared paste deadline — the only
 * way this file runs a helper whose output it consumes. An expired
 * deadline skips the helper outright, so one stall can't stack with
 * the fallback chain's later attempts (see CLIPBOARD_PASTE_TIMEOUT_MS
 * in clipboard.h). */
static char *paste_capture(const char *const *argv, size_t max, long deadline_ms, size_t *out_len)
{
    long left = deadline_ms - monotonic_ms();
    if (left <= 0)
        return NULL;
    return spawn_capture(argv, max, left > INT_MAX ? INT_MAX : (int)left, out_len);
}

/* macOS clipboard image via osascript — no extra install needed
 * (pbpaste is text-only). AppleScript can't stream binary to stdout,
 * so the script writes the clipboard's PNG rendition to a scratch file.
 * The scratch is consumed before return, so it is deliberately not in
 * the tracked-tempfile registry. macOS-only: on Linux osascript would
 * just ENOENT, so gating spares a pointless scratch file + failed exec
 * on every fall-through paste. */
#ifdef __APPLE__
static char *paste_image_osascript(size_t *out_len, long deadline_ms)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    char *path = path_join(tmp, "hax-clip-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }

    /* Escape for an AppleScript string literal ("\" and "\""). */
    struct buf esc;
    buf_init(&esc);
    for (const char *c = path; *c; c++) {
        if (*c == '\\' || *c == '"')
            buf_append(&esc, "\\", 1);
        buf_append(&esc, c, 1);
    }
    char *esc_path = buf_steal(&esc);
    /* \xc2\xab / \xc2\xbb are the UTF-8 guillemets in the raw-class
     * syntax for the PNG clipboard flavor. */
    char *script = xasprintf("set f to open for access POSIX file \"%s\" with write permission\n"
                             "write (the clipboard as \xc2\xab"
                             "class PNGf\xc2\xbb) to f\n"
                             "close access f",
                             esc_path);
    free(esc_path);

    const char *argv[] = {"osascript", "-e", script, NULL};
    size_t junk = 0;
    char *out = paste_capture(argv, CLIPBOARD_IMAGE_MAX_BYTES, deadline_ms, &junk);
    free(out); /* osascript's stdout is script chatter, not the image */
    free(script);

    /* Empty scratch file = script failed (no image on the clipboard,
     * or no osascript on this platform). */
    char *data = NULL;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size <= CLIPBOARD_IMAGE_MAX_BYTES) {
        size_t n = (size_t)st.st_size;
        data = xmalloc(n);
        ssize_t got = 0;
        size_t off = 0;
        while (off < n && (got = pread(fd, data + off, n - off, (off_t)off)) > 0)
            off += (size_t)got;
        if (off != n) {
            free(data);
            data = NULL;
        } else {
            *out_len = n;
        }
    }
    close(fd);
    unlink(path);
    free(path);
    return data;
}
#else
/* Wayland/X11 image negotiation. On macOS the native pasteboard
 * (osascript above) is authoritative, so these aren't compiled. */

/* Pick the best image type from a newline-separated offer listing
 * (wl-paste --list-types / xclip TARGETS): image/png when available,
 * else the first type the image path supports. Returns a static string
 * or NULL when nothing usable is on offer. */
static const char *pick_image_type(const char *types)
{
    static const char *const SUPPORTED[] = {"image/png", "image/jpeg", "image/gif", "image/webp"};
    const char *best = NULL;
    size_t best_rank = sizeof(SUPPORTED) / sizeof(SUPPORTED[0]);
    const char *p = types;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        for (size_t i = 0; i < best_rank; i++) {
            if (n == strlen(SUPPORTED[i]) && strncmp(p, SUPPORTED[i], n) == 0) {
                best = SUPPORTED[i];
                best_rank = i;
                break;
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return best;
}

/* List the clipboard's offered types via `lister`, pick a supported
 * image type, and fetch it by substituting the type for the TYPE
 * placeholder in `fetcher`. Never requests an unpinned type: helpers
 * asked for "whatever you have" can serve raw image bytes as text.
 * *listed is set once the lister produces an offer listing, so the
 * caller can tell "this backend answered: no image" (authoritative)
 * from "this backend is unavailable" (try the next one). */
static char *paste_image_negotiated(const char *const *lister, const char *const *fetcher,
                                    size_t *out_len, int *listed, long deadline_ms)
{
    size_t tn = 0;
    char *types = paste_capture(lister, 65536, deadline_ms, &tn);
    if (!types)
        return NULL;
    *listed = 1;
    const char *t = pick_image_type(types);
    free(types);
    if (!t)
        return NULL;

    const char *argv[8];
    size_t i = 0;
    for (; fetcher[i] && i < 7; i++)
        argv[i] = strcmp(fetcher[i], "TYPE") == 0 ? t : fetcher[i];
    argv[i] = NULL;
    return paste_capture(argv, CLIPBOARD_IMAGE_MAX_BYTES, deadline_ms, out_len);
}
#endif /* __APPLE__ */

char *clipboard_paste_image(size_t *out_len, long deadline_ms)
{
#ifdef __APPLE__
    /* Native pasteboard first, matching pbcopy/pbpaste priority: an
     * XQuartz X11 selection is a separate surface, so consulting xclip
     * here could shadow the real clipboard's image with stale X11
     * content once its listing became authoritative. */
    return paste_image_osascript(out_len, deadline_ms);
#else
    /* Wayland first when the compositor is reachable, same ordering
     * rationale as try_native: under XWayland both may answer, but the
     * Wayland clipboard is the one the user actually copied into. */
    if (getenv("WAYLAND_DISPLAY")) {
        const char *lister[] = {"wl-paste", "--list-types", NULL};
        const char *fetcher[] = {"wl-paste", "-t", "TYPE", NULL};
        int listed = 0;
        char *out = paste_image_negotiated(lister, fetcher, out_len, &listed, deadline_ms);
        /* A successful listing is authoritative even when it offers no
         * image: under XWayland the X11 clipboard is a different,
         * possibly stale selection, and falling through would risk
         * pasting an unrelated old image (or stalling on a broken X11
         * owner) while valid Wayland text waits in the text fallback. */
        if (out || listed)
            return out;
    }
    {
        const char *lister[] = {"xclip", "-selection", "clipboard", "-t", "TARGETS", "-o", NULL};
        const char *fetcher[] = {"xclip", "-selection", "clipboard", "-t", "TYPE", "-o", NULL};
        int listed = 0;
        char *out = paste_image_negotiated(lister, fetcher, out_len, &listed, deadline_ms);
        if (out || listed)
            return out;
    }
    return NULL;
#endif
}

char *clipboard_paste_text(size_t *out_len, long deadline_ms)
{
    /* Same helper order as try_native's copy direction. */
    {
        const char *argv[] = {"pbpaste", NULL};
        char *out = paste_capture(argv, CLIPBOARD_TEXT_MAX_BYTES, deadline_ms, out_len);
        if (out)
            return out;
    }
    if (getenv("WAYLAND_DISPLAY")) {
        const char *lister[] = {"wl-paste", "--list-types", NULL};
        size_t tn = 0;
        char *types = paste_capture(lister, 65536, deadline_ms, &tn);
        if (types) {
            free(types);
            /* Wayland answered, so it is the clipboard authority —
             * same rationale as clipboard_paste_image: falling through
             * to xclip/xsel could serve stale XWayland content. Pin
             * the type: bare wl-paste auto-picks from the offer list
             * and would emit raw image bytes when no text is on offer;
             * "text" is wl-paste's documented any-text alias. */
            const char *argv[] = {"wl-paste", "-n", "-t", "text", NULL};
            return paste_capture(argv, CLIPBOARD_TEXT_MAX_BYTES, deadline_ms, out_len);
        }
    }
    {
        const char *argv[] = {"xclip", "-selection", "clipboard", "-o", NULL};
        char *out = paste_capture(argv, CLIPBOARD_TEXT_MAX_BYTES, deadline_ms, out_len);
        if (out)
            return out;
    }
    {
        const char *argv[] = {"xsel", "-b", "-o", NULL};
        char *out = paste_capture(argv, CLIPBOARD_TEXT_MAX_BYTES, deadline_ms, out_len);
        if (out)
            return out;
    }
    return NULL;
}

int clipboard_copy(const char *text, size_t len, const char **err)
{
    if (is_ssh()) {
        if (write_osc52(text, len) == 0)
            return 0;
        if (err) {
            *err = (len > CLIPBOARD_OSC52_MAX_BYTES) ? "response too large for OSC 52 over SSH"
                                                     : "terminal did not accept OSC 52 sequence";
        }
        return -1;
    }
    if (try_native(text, len) == 0)
        return 0;
    if (write_osc52(text, len) == 0)
        return 0;
    if (err)
        *err = "no clipboard helper available "
               "(install pbcopy / wl-copy / xclip / xsel, or use a terminal that supports OSC 52)";
    return -1;
}
