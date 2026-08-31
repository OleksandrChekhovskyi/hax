/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "harness.h"
#include "paste_image.h"
#include "xalloc.h"
#include "system/tempfiles.h"

static void expect_normalized_text(const char *input, size_t input_len, const char *expected)
{
    char *text = xmalloc(input_len + 1);
    memcpy(text, input, input_len);
    text[input_len] = '\0';
    size_t normalized_len = paste_image_normalize_text(text, input_len);
    EXPECT(normalized_len == strlen(expected));
    EXPECT_STR_EQ(text, expected);
    free(text);
}

static void test_normalize_crlf(void)
{
    expect_normalized_text("a\r\nb\r\n", 6, "a\nb\n");
}

static void test_normalize_lone_cr(void)
{
    expect_normalized_text("a\rb", 3, "a\nb");
    expect_normalized_text("\r", 1, "\n");
}

static void test_normalize_strips_nuls(void)
{
    expect_normalized_text("a\0b", 3, "ab");
}

static void test_normalize_plain_passthrough(void)
{
    expect_normalized_text("hello\nworld", 11, "hello\nworld");
    expect_normalized_text("", 0, "");
}

static void test_normalize_mixed(void)
{
    expect_normalized_text("x\r\n\0y\rz", 7, "x\ny\nz");
}

/* A complete 2x3 RGB PNG accepted by image_sniff(). */
static const unsigned char TINY_PNG[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x08, 0x02, 0x00, 0x00, 0x00, 0x36, 0x88, 0x49,
    0xd6, 0x00, 0x00, 0x00, 0x15, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x80, 0x00, 0x8d,
    0x80, 0x0a, 0x20, 0x62, 0x08, 0x58, 0xf0, 0x01, 0x88, 0x00, 0x1f, 0x05, 0x05, 0xa1, 0xfc, 0xf8,
    0x4b, 0x42, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

static void write_file(const char *path, const void *data, size_t n, mode_t mode)
{
    FILE *f = fopen(path, "wb");
    EXPECT(f != NULL);
    if (!f)
        return;
    EXPECT(fwrite(data, 1, n, f) == n);
    fclose(f);
    chmod(path, mode);
}

/* Image fetches must request FAKE_IMG_MIME exactly. FAKE_X11_* overrides the default clipboard for
 * X11 helpers, and the osascript form copies FAKE_IMG_FILE to its script's scratch path. */
static void install_fake_helpers(const char *dir)
{
    static const char SCRIPT[] =
        "#!/bin/sh\n"
        "case \"$0\" in *xclip|*xsel)\n"
        "  [ -n \"$FAKE_X11_IMG_MIME\" ] && "
        "FAKE_IMG_MIME=$FAKE_X11_IMG_MIME FAKE_IMG_FILE=$FAKE_X11_IMG_FILE\n"
        "  [ -n \"$FAKE_X11_TEXT\" ] && FAKE_TEXT=$FAKE_X11_TEXT\n"
        ";; esac\n"
        "if [ \"$1\" = -e ]; then\n" /* osascript shape */
        "  scratch=$(printf '%s\\n' \"$2\" | "
        "sed -n 's/.*POSIX file \"\\([^\"]*\\)\".*/\\1/p')\n"
        "  [ -n \"$FAKE_IMG_FILE\" ] && [ -n \"$scratch\" ] && "
        "cat \"$FAKE_IMG_FILE\" > \"$scratch\"\n"
        "  exit 0\n"
        "fi\n"
        "list=no mode=text want=\n"
        "for a in \"$@\"; do\n"
        "  [ \"$a\" = --list-types ] && list=yes\n" /* wl-paste */
        "  [ \"$a\" = TARGETS ] && list=yes\n"      /* xclip */
        "  case \"$a\" in image/*) mode=img want=\"$a\";; esac\n"
        "done\n"
        "if [ \"$list\" = yes ]; then\n"
        "  [ -n \"$FAKE_IMG_MIME\" ] && printf '%s\\n' \"$FAKE_IMG_MIME\"\n"
        "  [ -n \"$FAKE_TEXT\" ] && printf 'text/plain\\n'\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$mode\" = img ]; then\n"
        "  [ -n \"$FAKE_IMG_FILE\" ] && [ \"$want\" = \"$FAKE_IMG_MIME\" ] && "
        "exec cat \"$FAKE_IMG_FILE\"\n"
        "  exit 1\n"
        "fi\n"
        "[ -n \"$FAKE_TEXT\" ] && exec printf '%s' \"$FAKE_TEXT\"\n"
        "exit 1\n";
    static const char *const NAMES[] = {"wl-paste", "xclip", "xsel", "pbpaste", "osascript"};
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        char *path = xasprintf("%s/%s", dir, NAMES[i]);
        write_file(path, SCRIPT, sizeof(SCRIPT) - 1, 0755);
        free(path);
    }
}

/* Extract the path between "[pasted image: " and "]". Caller frees. */
static char *marker_path(const char *marker)
{
    static const char PREFIX[] = "[pasted image: ";
    if (strncmp(marker, PREFIX, sizeof(PREFIX) - 1) != 0)
        return NULL;
    const char *start = marker + sizeof(PREFIX) - 1;
    const char *end = strchr(start, ']');
    if (!end)
        return NULL;
    char *path = xmalloc((size_t)(end - start) + 1);
    memcpy(path, start, (size_t)(end - start));
    path[end - start] = '\0';
    return path;
}

static void test_capture_image_to_marker(void)
{
    char *img = xasprintf("%s/clip.png", t_tempdir());
    write_file(img, TINY_PNG, sizeof(TINY_PNG), 0644);
    setenv("FAKE_IMG_FILE", img, 1);
    setenv("FAKE_IMG_MIME", "image/png", 1);
    unsetenv("FAKE_TEXT");

    char *marker = paste_image_capture();
    EXPECT(marker != NULL);
    if (marker) {
        EXPECT(marker[strlen(marker) - 1] == ' ');
        char *path = marker_path(marker);
        EXPECT(path != NULL);
        if (path) {
            EXPECT(strstr(path, "/hax-") != NULL);
            EXPECT(strstr(path, "/paste-") != NULL);
            EXPECT(strlen(path) > 4 && strcmp(path + strlen(path) - 4, ".png") == 0);
            struct stat st;
            EXPECT(stat(path, &st) == 0 && (size_t)st.st_size == sizeof(TINY_PNG));
            tempfiles_cleanup();
            EXPECT(stat(path, &st) < 0);
            free(path);
        }
        free(marker);
    }
    free(img);
}

static void test_capture_garbage_image_falls_back_to_text(void)
{
    char *img = xasprintf("%s/garbage.png", t_tempdir());
    write_file(img, TINY_PNG, 16, 0644);
    setenv("FAKE_IMG_FILE", img, 1);
    setenv("FAKE_IMG_MIME", "image/png", 1);
    setenv("FAKE_TEXT", "hi\r\nthere", 1);

    char *out = paste_image_capture();
    EXPECT(out != NULL);
    if (out) {
        EXPECT_STR_EQ(out, "hi\nthere");
        free(out);
    }
    free(img);
}

static void test_capture_empty_clipboard_returns_null(void)
{
    unsetenv("FAKE_IMG_FILE");
    unsetenv("FAKE_IMG_MIME");
    unsetenv("FAKE_TEXT");
    EXPECT(paste_image_capture() == NULL);
}

static void test_capture_negotiates_non_png_type(void)
{
    static const unsigned char TINY_GIF[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xff, 0xff, 0xff, 0x21, 0xf9, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b,
    };
    char *img = xasprintf("%s/clip.gif", t_tempdir());
    write_file(img, TINY_GIF, sizeof(TINY_GIF), 0644);
    setenv("FAKE_IMG_FILE", img, 1);
    setenv("FAKE_IMG_MIME", "image/gif", 1);
    unsetenv("FAKE_TEXT");

    char *marker = paste_image_capture();
    EXPECT(marker != NULL);
    if (marker) {
        char *path = marker_path(marker);
        EXPECT(path != NULL);
        if (path) {
            EXPECT(strlen(path) > 4 && strcmp(path + strlen(path) - 4, ".gif") == 0);
            free(path);
        }
        free(marker);
        tempfiles_cleanup();
    }
    free(img);
}

static void test_capture_persist_failure_falls_back_to_text(void)
{
    const char *current_tmpdir = getenv("TMPDIR");
    char *saved_tmpdir = current_tmpdir ? xstrdup(current_tmpdir) : NULL;

    char *img = xasprintf("%s/clip.png", t_tempdir());
    write_file(img, TINY_PNG, sizeof(TINY_PNG), 0644);
    setenv("FAKE_IMG_FILE", img, 1);
    setenv("FAKE_IMG_MIME", "image/png", 1);
    setenv("FAKE_TEXT", "fallback text", 1);
    setenv("TMPDIR", "/nonexistent-hax/xyz", 1);

    char *out = paste_image_capture();
    EXPECT(out != NULL);
    if (out) {
        EXPECT_STR_EQ(out, "fallback text");
        free(out);
    }

    if (saved_tmpdir) {
        setenv("TMPDIR", saved_tmpdir, 1);
        free(saved_tmpdir);
    } else {
        unsetenv("TMPDIR");
    }
    unsetenv("FAKE_TEXT");
    free(img);
}

static void test_capture_wayland_prevents_x11_image_fallback(void)
{
    char *img = xasprintf("%s/stale.png", t_tempdir());
    write_file(img, TINY_PNG, sizeof(TINY_PNG), 0644);
    unsetenv("FAKE_IMG_MIME");
    unsetenv("FAKE_IMG_FILE");
    setenv("FAKE_TEXT", "current wayland text", 1);
    setenv("FAKE_X11_IMG_MIME", "image/png", 1);
    setenv("FAKE_X11_IMG_FILE", img, 1);

    char *out = paste_image_capture();
    EXPECT(out != NULL);
    if (out) {
        EXPECT_STR_EQ(out, "current wayland text");
        free(out);
    }
    unsetenv("FAKE_X11_IMG_MIME");
    unsetenv("FAKE_X11_IMG_FILE");
    unsetenv("FAKE_TEXT");
    free(img);
}

static void test_capture_wayland_prevents_x11_text_fallback(void)
{
    setenv("FAKE_IMG_MIME", "image/bmp", 1);
    unsetenv("FAKE_IMG_FILE");
    unsetenv("FAKE_TEXT");
    setenv("FAKE_X11_TEXT", "stale x11 text", 1);

    EXPECT(paste_image_capture() == NULL);

    unsetenv("FAKE_X11_TEXT");
    unsetenv("FAKE_IMG_MIME");
}

static void test_uris_plain_file(void)
{
    char *out = paste_image_uris_to_paths("file:///etc/hostname");
    EXPECT(out != NULL);
    if (out) {
        EXPECT_STR_EQ(out, "/etc/hostname ");
        free(out);
    }
}

static void test_uris_percent_decode_and_localhost(void)
{
    char *out = paste_image_uris_to_paths("file://localhost/a%20dir/b%2Bc.txt");
    EXPECT(out != NULL);
    if (out) {
        EXPECT_STR_EQ(out, "/a dir/b+c.txt ");
        free(out);
    }
}

static void test_uris_image_extension_gets_marker_without_file_access(void)
{
    char *out = paste_image_uris_to_paths("file:///no/such/dir/pic.PNG");
    EXPECT(out != NULL);
    if (out) {
        EXPECT_STR_EQ(out, "[pasted image: /no/such/dir/pic.PNG] ");
        free(out);
    }
}

static void test_uris_multiple_lines(void)
{
    char *out = paste_image_uris_to_paths("file:///a\nfile:///b\n");
    EXPECT(out != NULL);
    if (out) {
        EXPECT_STR_EQ(out, "/a\n/b ");
        free(out);
    }
}

static void test_uris_fifo_is_not_opened(void)
{
    /* Opening this writer-less FIFO would hang the test. */
    char *fifo = xasprintf("%s/pipe.png", t_tempdir());
    EXPECT(mkfifo(fifo, 0600) == 0);
    char *uri = xasprintf("file://%s", fifo);
    char *out = paste_image_uris_to_paths(uri);
    EXPECT(out != NULL);
    if (out) {
        char *want = xasprintf("[pasted image: %s] ", fifo);
        EXPECT_STR_EQ(out, want);
        free(want);
        free(out);
    }
    free(uri);
    free(fifo);
}

static void test_uris_reject_non_uri_text(void)
{
    EXPECT(paste_image_uris_to_paths("hello world") == NULL);
    EXPECT(paste_image_uris_to_paths("file:///a\nnot a uri") == NULL);
    EXPECT(paste_image_uris_to_paths("https://example.com/x.png") == NULL);
    EXPECT(paste_image_uris_to_paths("file://remotehost/share/x") == NULL);
    EXPECT(paste_image_uris_to_paths("file://") == NULL);
    EXPECT(paste_image_uris_to_paths("") == NULL);
    EXPECT(paste_image_uris_to_paths("file:///tmp/a%00.png") == NULL);
}

int main(void)
{
    test_normalize_crlf();
    test_normalize_lone_cr();
    test_normalize_strips_nuls();
    test_normalize_plain_passthrough();
    test_normalize_mixed();

    char *helpers = t_tempdir();
    install_fake_helpers(helpers);
    char *path_env = xasprintf("%s:%s", helpers, getenv("PATH"));
    setenv("PATH", path_env, 1);
    free(path_env);
    setenv("TMPDIR", t_tempdir(), 1);
    setenv("WAYLAND_DISPLAY", "fake-0", 1);

    test_capture_image_to_marker();
    test_capture_garbage_image_falls_back_to_text();
    test_capture_empty_clipboard_returns_null();
    test_capture_negotiates_non_png_type();
    test_capture_persist_failure_falls_back_to_text();
    test_capture_wayland_prevents_x11_image_fallback();
    test_capture_wayland_prevents_x11_text_fallback();

    test_uris_plain_file();
    test_uris_percent_decode_and_localhost();
    test_uris_image_extension_gets_marker_without_file_access();
    test_uris_multiple_lines();
    test_uris_fifo_is_not_opened();
    test_uris_reject_non_uri_text();
    T_REPORT();
}
