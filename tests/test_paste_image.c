/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "paste_image.h"
#include "util.h"
#include "system/tempfiles.h"

static void check(const char *in, size_t n, const char *want)
{
    char *s = xmalloc(n + 1);
    memcpy(s, in, n);
    s[n] = '\0';
    size_t got = paste_image_normalize_text(s, n);
    EXPECT(got == strlen(want));
    EXPECT_STR_EQ(s, want);
    free(s);
}

static void test_normalize_crlf(void)
{
    check("a\r\nb\r\n", 6, "a\nb\n");
}

static void test_normalize_lone_cr(void)
{
    check("a\rb", 3, "a\nb");
    check("\r", 1, "\n");
}

static void test_normalize_strips_nuls(void)
{
    check("a\0b", 3, "ab");
}

static void test_normalize_plain_passthrough(void)
{
    check("hello\nworld", 11, "hello\nworld");
    check("", 0, "");
}

static void test_normalize_mixed(void)
{
    /* CRLF, lone CR, and NUL in one buffer. */
    check("x\r\n\0y\rz", 7, "x\ny\nz");
}

/* ---- end-to-end capture via fake clipboard helpers ----
 *
 * paste_image_capture shells out to wl-paste/xclip/xsel/pbpaste/
 * osascript; a directory of fake helpers prepended to PATH makes the
 * flow deterministic with no real clipboard (see install_fake_helpers
 * for the fake's contract). osascript's invocation matches neither the
 * listing nor the fetch shape and exits 1, exercising its "scratch
 * file stayed empty" failure path. */

/* Same decodable 2x3 RGB PNG fixture as tools/test_read.c — the sniff
 * in paste_image_capture requires a complete container. */
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

/* The fake advertises $FAKE_IMG_MIME in its type listing (plus
 * text/plain when $FAKE_TEXT is set), serves $FAKE_IMG_FILE only when
 * that exact type is requested — a pinned-type request for anything
 * else fails like the real helpers — and serves $FAKE_TEXT for text
 * requests. When invoked as xclip/xsel with $FAKE_X11_IMG_MIME or
 * $FAKE_X11_TEXT set, those override the Wayland content, simulating
 * the divergent stale X11 clipboard XWayland exposes next to the live
 * Wayland selection. As osascript (-e <script>) it plays the macOS-only
 * path: extract the scratch path the production AppleScript opens and
 * deliver $FAKE_IMG_FILE there, so the osascript backend is exercised
 * when clipboard.c is compiled under __APPLE__. */
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
        /* Trailing space so the user can keep typing. */
        EXPECT(marker[strlen(marker) - 1] == ' ');
        char *path = marker_path(marker);
        EXPECT(path != NULL);
        if (path) {
            /* Container dir + readable name + sniffed extension. */
            EXPECT(strstr(path, "/hax-") != NULL);
            EXPECT(strstr(path, "/paste-") != NULL);
            EXPECT(strlen(path) > 4 && strcmp(path + strlen(path) - 4, ".png") == 0);
            struct stat st;
            EXPECT(stat(path, &st) == 0 && (size_t)st.st_size == sizeof(TINY_PNG));
            /* The tracked-registry flush must reclaim it. */
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
    /* Helper "succeeds" but the bytes aren't a complete image (bare PNG
     * signature): sniff refuses, text fallback wins, CRLF normalized. */
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
    /* A clipboard offering only image/gif (no PNG rendition, no text)
     * must still paste: the offer listing drives the requested type,
     * and the marker gets the sniffed extension. Before type
     * negotiation this fell through to the unpinned text fallback,
     * which could serve the raw image bytes as "text". */
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
    /* The image sniffs clean but can't be persisted (container creation
     * fails under a nonexistent $TMPDIR); the paste must fall back to the
     * clipboard's text rather than becoming a no-op. */
    const char *cur = getenv("TMPDIR");
    char *saved = cur ? xstrdup(cur) : NULL;

    char *img = xasprintf("%s/clip.png", t_tempdir()); /* create before breaking TMPDIR */
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

    if (saved) {
        setenv("TMPDIR", saved, 1);
        free(saved);
    } else {
        unsetenv("TMPDIR");
    }
    unsetenv("FAKE_TEXT");
    free(img);
}

static void test_capture_wayland_no_image_is_authoritative(void)
{
    /* The Wayland listing succeeds and offers text only, while the
     * (stale) X11 clipboard still offers an image. The Wayland verdict
     * must win: paste the current text, never fetch the old X11 image. */
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

static void test_capture_wayland_authority_covers_text(void)
{
    /* The Wayland clipboard offers only an unsupported image type and
     * no text; the stale X11 clipboard still holds text. The Wayland
     * verdict must carry through the text fallback too: paste nothing
     * rather than unrelated X11 content. */
    setenv("FAKE_IMG_MIME", "image/bmp", 1);
    unsetenv("FAKE_IMG_FILE");
    unsetenv("FAKE_TEXT");
    setenv("FAKE_X11_TEXT", "stale x11 text", 1);

    EXPECT(paste_image_capture() == NULL);

    unsetenv("FAKE_X11_TEXT");
    unsetenv("FAKE_IMG_MIME");
}

/* ---- file:// URI conversion (Dolphin/Nautilus copy, drag-and-drop) ---- */

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

static void test_uris_image_gets_marker(void)
{
    /* The marker is driven by extension alone — no file access — so a
     * path that doesn't exist still gets marked (case-insensitively).
     * This is what keeps the raw-mode editor off a stalled NFS/FUSE
     * path or a writer-less FIFO named like an image. */
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
    /* Regression guard against reintroducing file I/O: a writer-less
     * FIFO with an image extension exercises the marker branch, which
     * must classify by extension without opening the file — an fopen
     * here would block forever and the test would hang (timeout). */
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
    /* A decoded %00 would truncate the path via strlen downstream —
     * reject rather than convert. */
    EXPECT(paste_image_uris_to_paths("file:///tmp/a%00.png") == NULL);
}

int main(void)
{
    test_normalize_crlf();
    test_normalize_lone_cr();
    test_normalize_strips_nuls();
    test_normalize_plain_passthrough();
    test_normalize_mixed();

    /* Fake-helper PATH + private TMPDIR for the capture tests. */
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
    test_capture_wayland_no_image_is_authoritative();
    test_capture_wayland_authority_covers_text();

    test_uris_plain_file();
    test_uris_percent_decode_and_localhost();
    test_uris_image_gets_marker();
    test_uris_multiple_lines();
    test_uris_fifo_is_not_opened();
    test_uris_reject_non_uri_text();
    T_REPORT();
}
