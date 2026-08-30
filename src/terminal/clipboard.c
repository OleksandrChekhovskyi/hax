/* SPDX-License-Identifier: MIT */
#include "terminal/clipboard.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* fstat backs the __APPLE__-only osascript path, which the Linux lint pass
 * cannot see; the wait macros are used unconditionally but glibc also leaks
 * them through <stdlib.h>, so the include cleaner cannot attribute them. */
#include <sys/stat.h> // IWYU pragma: keep
#include <sys/wait.h> // IWYU pragma: keep

#include "util.h"
/* The buf builders are __APPLE__-only here, invisible to the Linux lint pass. */
#include "buf.h" // IWYU pragma: keep
#include "system/fs.h"
/* path_join is __APPLE__-only here, invisible to the Linux lint pass. */
#include "system/path.h" // IWYU pragma: keep
#include "system/spawn.h"
#include "terminal/ansi.h"
#include "text/base64.h"

/* Cap untrusted helper output before it reaches the editor or image decoder. */
#define CLIPBOARD_IMAGE_MAX_BYTES (64u << 20)
#define CLIPBOARD_TEXT_MAX_BYTES  (1u << 20)
#define CLIPBOARD_TYPES_MAX_BYTES (64u << 10)

#define OSC52_PREFIX      ANSI_ESC "]52;c;"
#define OSC52_SUFFIX      ANSI_BEL
#define TMUX_OSC52_PREFIX ANSI_TMUX_PASSTHROUGH_BEGIN OSC52_PREFIX
#define TMUX_OSC52_SUFFIX OSC52_SUFFIX ANSI_TMUX_PASSTHROUGH_END

char *clipboard_osc52_sequence(const char *text, size_t text_len, int tmux_wrap, size_t *out_len)
{
    if (text_len > CLIPBOARD_OSC52_MAX_BYTES)
        return NULL;

    size_t encoded_len;
    char *encoded = base64_encode(text, text_len, &encoded_len);
    const char *prefix = tmux_wrap ? TMUX_OSC52_PREFIX : OSC52_PREFIX;
    const char *suffix = tmux_wrap ? TMUX_OSC52_SUFFIX : OSC52_SUFFIX;
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    size_t sequence_len = prefix_len + encoded_len + suffix_len;
    char *sequence = xmalloc(sequence_len + 1);

    memcpy(sequence, prefix, prefix_len);
    memcpy(sequence + prefix_len, encoded, encoded_len);
    memcpy(sequence + prefix_len + encoded_len, suffix, suffix_len + 1);
    free(encoded);

    if (out_len)
        *out_len = sequence_len;
    return sequence;
}

/* Use argv-based exec so probing PATH never invokes a shell. */
static int run_copy_helper(const char *const *argv, const char *text, size_t text_len)
{
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0)
        return -1;

    struct spawn_signal_state signals;
    spawn_parent_ignore_signals(&signals);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        spawn_parent_restore_signals(&signals);
        return -1;
    }
    if (pid == 0) {
        close(pipe_fds[1]);
        if (pipe_fds[0] != STDIN_FILENO) {
            if (dup2(pipe_fds[0], STDIN_FILENO) < 0)
                _exit(127);
            close(pipe_fds[0]);
        }

        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        spawn_child_reset_signals();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(pipe_fds[0]);
    int write_status = write_all(pipe_fds[1], text, text_len);
    close(pipe_fds[1]);
    int status = spawn_wait_child(pid);
    spawn_parent_restore_signals(&signals);
    if (status < 0 || write_status < 0)
        return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int copy_with_native_helper(const char *text, size_t text_len)
{
    /* pbcopy is also available on some non-macOS systems, so probe PATH everywhere. */
    {
        const char *argv[] = {"pbcopy", NULL};
        if (run_copy_helper(argv, text, text_len) == 0)
            return 0;
    }
    /* Prefer the Wayland clipboard over the potentially stale XWayland selection. */
    if (getenv("WAYLAND_DISPLAY")) {
        const char *argv[] = {"wl-copy", NULL};
        if (run_copy_helper(argv, text, text_len) == 0)
            return 0;
    }
    {
        const char *argv[] = {"xclip", "-selection", "clipboard", NULL};
        if (run_copy_helper(argv, text, text_len) == 0)
            return 0;
    }
    {
        const char *argv[] = {"xsel", "-b", "-i", NULL};
        if (run_copy_helper(argv, text, text_len) == 0)
            return 0;
    }
    return -1;
}

static int copy_with_osc52(const char *text, size_t text_len)
{
    int tmux_wrap = getenv("TMUX") != NULL;
    size_t sequence_len;
    char *sequence = clipboard_osc52_sequence(text, text_len, tmux_wrap, &sequence_len);
    if (!sequence)
        return -1;

    /* Write to the terminal even when stdout is redirected. */
    int fd = open("/dev/tty", O_WRONLY | O_NOCTTY);
    int owns_fd = fd >= 0;
    if (!owns_fd)
        fd = STDOUT_FILENO;
    int result = write_all(fd, sequence, sequence_len);
    if (owns_fd)
        close(fd);
    free(sequence);
    return result;
}

static int is_ssh_session(void)
{
    return getenv("SSH_TTY") != NULL || getenv("SSH_CONNECTION") != NULL;
}

/* Every helper consumes the same absolute deadline. */
static char *capture_helper_output(const char *const *argv, size_t max_bytes, long deadline_ms,
                                   size_t *out_len)
{
    long remaining_ms = deadline_ms - monotonic_ms();
    if (remaining_ms <= 0)
        return NULL;
    int timeout_ms = remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
    return spawn_capture_stdout(argv, max_bytes, timeout_ms, out_len);
}

#ifdef __APPLE__
/* AppleScript writes binary clipboard data through a file rather than stdout. */
static char *paste_image_with_osascript(size_t *out_len, long deadline_ms)
{
    const char *temp_dir = getenv("TMPDIR");
    if (!temp_dir || !*temp_dir)
        temp_dir = "/tmp";
    char *path = path_join(temp_dir, "hax-clip-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }

    /* Escape backslashes and double quotes for an AppleScript string literal. */
    struct buf escaped;
    buf_init(&escaped);
    for (const char *cursor = path; *cursor; cursor++) {
        if (*cursor == '\\' || *cursor == '"')
            buf_append(&escaped, "\\", 1);
        buf_append(&escaped, cursor, 1);
    }
    char *escaped_path = buf_steal(&escaped);
    /* The UTF-8 guillemets delimit AppleScript's raw PNG class name. */
    char *script = xasprintf("set f to open for access POSIX file \"%s\" with write permission\n"
                             "write (the clipboard as \xc2\xab"
                             "class PNGf\xc2\xbb) to f\n"
                             "close access f",
                             escaped_path);
    free(escaped_path);

    const char *argv[] = {"osascript", "-e", script, NULL};
    size_t ignored_len;
    char *helper_output =
        capture_helper_output(argv, CLIPBOARD_IMAGE_MAX_BYTES, deadline_ms, &ignored_len);
    free(helper_output);
    free(script);

    char *image = NULL;
    struct stat status;
    if (fstat(fd, &status) == 0 && status.st_size > 0 &&
        (size_t)status.st_size <= CLIPBOARD_IMAGE_MAX_BYTES) {
        size_t image_len = (size_t)status.st_size;
        image = xmalloc(image_len);
        ssize_t bytes_read = 0;
        size_t offset = 0;
        while (offset < image_len &&
               (bytes_read = pread(fd, image + offset, image_len - offset, (off_t)offset)) > 0) {
            offset += (size_t)bytes_read;
        }
        if (offset != image_len) {
            free(image);
            image = NULL;
        } else {
            *out_len = image_len;
        }
    }
    close(fd);
    unlink(path);
    free(path);
    return image;
}
#else

/* Return the most preferred supported MIME type, borrowed from a static table. */
static const char *pick_image_mime_type(const char *offered_types)
{
    static const char *const SUPPORTED_TYPES[] = {"image/png", "image/jpeg", "image/gif",
                                                  "image/webp"};
    const char *selected_type = NULL;
    size_t selected_rank = sizeof(SUPPORTED_TYPES) / sizeof(SUPPORTED_TYPES[0]);
    const char *line = offered_types;

    while (*line) {
        const char *newline = strchr(line, '\n');
        size_t line_len = newline ? (size_t)(newline - line) : strlen(line);
        for (size_t rank = 0; rank < selected_rank; rank++) {
            const char *supported_type = SUPPORTED_TYPES[rank];
            if (line_len == strlen(supported_type) &&
                strncmp(line, supported_type, line_len) == 0) {
                selected_type = supported_type;
                selected_rank = rank;
                break;
            }
        }
        if (!newline)
            break;
        line = newline + 1;
    }
    return selected_type;
}

/* `listing_succeeded` distinguishes an authoritative no-image result from an unavailable helper. */
static const char *list_image_mime_type(const char *const *argv, int *listing_succeeded,
                                        long deadline_ms)
{
    size_t listing_len;
    char *listing =
        capture_helper_output(argv, CLIPBOARD_TYPES_MAX_BYTES, deadline_ms, &listing_len);
    if (!listing)
        return NULL;

    if (listing_succeeded)
        *listing_succeeded = 1;
    const char *mime_type = pick_image_mime_type(listing);
    free(listing);
    return mime_type;
}
#endif /* __APPLE__ */

char *clipboard_paste_image(size_t *out_len, long deadline_ms)
{
#ifdef __APPLE__
    return paste_image_with_osascript(out_len, deadline_ms);
#else
    /* Prefer the Wayland clipboard over the potentially stale XWayland selection. */
    if (getenv("WAYLAND_DISPLAY")) {
        const char *list_argv[] = {"wl-paste", "--list-types", NULL};
        int listing_succeeded = 0;
        const char *mime_type = list_image_mime_type(list_argv, &listing_succeeded, deadline_ms);
        if (mime_type) {
            const char *read_argv[] = {"wl-paste", "-t", mime_type, NULL};
            return capture_helper_output(read_argv, CLIPBOARD_IMAGE_MAX_BYTES, deadline_ms,
                                         out_len);
        }
        if (listing_succeeded)
            return NULL;
    }

    const char *list_argv[] = {"xclip", "-selection", "clipboard", "-t", "TARGETS", "-o", NULL};
    const char *mime_type = list_image_mime_type(list_argv, NULL, deadline_ms);
    if (!mime_type)
        return NULL;
    const char *read_argv[] = {"xclip", "-selection", "clipboard", "-t", mime_type, "-o", NULL};
    return capture_helper_output(read_argv, CLIPBOARD_IMAGE_MAX_BYTES, deadline_ms, out_len);
#endif
}

char *clipboard_paste_text(size_t *out_len, long deadline_ms)
{
    {
        const char *argv[] = {"pbpaste", NULL};
        char *text = capture_helper_output(argv, CLIPBOARD_TEXT_MAX_BYTES, deadline_ms, out_len);
        if (text)
            return text;
    }
    if (getenv("WAYLAND_DISPLAY")) {
        /* A responding Wayland clipboard is authoritative over the XWayland selection. */
        const char *list_argv[] = {"wl-paste", "--list-types", NULL};
        size_t listing_len;
        char *listing =
            capture_helper_output(list_argv, CLIPBOARD_TYPES_MAX_BYTES, deadline_ms, &listing_len);
        if (listing) {
            free(listing);
            /* Pin the documented any-text alias; untyped wl-paste may select raw image bytes. */
            const char *read_argv[] = {"wl-paste", "-n", "-t", "text", NULL};
            return capture_helper_output(read_argv, CLIPBOARD_TEXT_MAX_BYTES, deadline_ms, out_len);
        }
    }
    {
        const char *argv[] = {"xclip", "-selection", "clipboard", "-o", NULL};
        char *text = capture_helper_output(argv, CLIPBOARD_TEXT_MAX_BYTES, deadline_ms, out_len);
        if (text)
            return text;
    }
    {
        const char *argv[] = {"xsel", "-b", "-o", NULL};
        char *text = capture_helper_output(argv, CLIPBOARD_TEXT_MAX_BYTES, deadline_ms, out_len);
        if (text)
            return text;
    }
    return NULL;
}

int clipboard_copy(const char *text, size_t text_len, const char **error)
{
    if (is_ssh_session()) {
        if (copy_with_osc52(text, text_len) == 0)
            return 0;
        if (error) {
            *error = text_len > CLIPBOARD_OSC52_MAX_BYTES
                         ? "response too large for OSC 52 over SSH"
                         : "terminal did not accept OSC 52 sequence";
        }
        return -1;
    }
    if (copy_with_native_helper(text, text_len) == 0)
        return 0;
    if (copy_with_osc52(text, text_len) == 0)
        return 0;
    if (error)
        *error =
            "no clipboard helper available "
            "(install pbcopy / wl-copy / xclip / xsel, or use a terminal that supports OSC 52)";
    return -1;
}
