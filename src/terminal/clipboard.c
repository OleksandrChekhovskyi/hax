/* SPDX-License-Identifier: MIT */
#include "terminal/clipboard.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "util.h"
#include "system/spawn.h"

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const char *data, size_t len, size_t *out_len)
{
    size_t out_n = ((len + 2) / 3) * 4;
    char *buf = xmalloc(out_n + 1);
    const unsigned char *in = (const unsigned char *)data;
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        buf[o++] = B64_ALPHABET[(v >> 18) & 0x3f];
        buf[o++] = B64_ALPHABET[(v >> 12) & 0x3f];
        buf[o++] = B64_ALPHABET[(v >> 6) & 0x3f];
        buf[o++] = B64_ALPHABET[v & 0x3f];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)in[i + 1] << 8;
        buf[o++] = B64_ALPHABET[(v >> 18) & 0x3f];
        buf[o++] = B64_ALPHABET[(v >> 12) & 0x3f];
        buf[o++] = (i + 1 < len) ? B64_ALPHABET[(v >> 6) & 0x3f] : '=';
        buf[o++] = '=';
    }
    buf[o] = '\0';
    if (out_len)
        *out_len = o;
    return buf;
}

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
