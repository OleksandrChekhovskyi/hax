/* SPDX-License-Identifier: MIT */
#ifdef _WIN32

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "util.h"
#include "terminal/ansi.h"
#include "terminal/clipboard.h"
#include "text/base64.h"

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

static wchar_t *utf8_to_wide(const char *text, size_t length, size_t *wide_length)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, NULL, 0);
    if (count <= 0)
        return NULL;
    wchar_t *wide = xmalloc(((size_t)count + 1) * sizeof(*wide));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, wide, count)) {
        free(wide);
        return NULL;
    }
    wide[count] = L'\0';
    if (wide_length)
        *wide_length = (size_t)count;
    return wide;
}

int clipboard_copy(const char *text, size_t text_len, const char **error)
{
    if (error)
        *error = NULL;
    size_t wide_length;
    wchar_t *wide = utf8_to_wide(text, text_len, &wide_length);
    if (!wide) {
        if (error)
            *error = "clipboard text is not valid UTF-8";
        return -1;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide_length + 1) * sizeof(*wide));
    if (!memory) {
        free(wide);
        return -1;
    }
    void *destination = GlobalLock(memory);
    memcpy(destination, wide, (wide_length + 1) * sizeof(*wide));
    GlobalUnlock(memory);
    free(wide);

    if (!OpenClipboard(NULL)) {
        GlobalFree(memory);
        return -1;
    }
    EmptyClipboard();
    HANDLE placed = SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!placed) {
        GlobalFree(memory);
        return -1;
    }
    return 0;
}

char *clipboard_paste_text(size_t *out_len, long deadline_ms)
{
    (void)deadline_ms;
    if (!OpenClipboard(NULL))
        return NULL;
    HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *wide = memory ? GlobalLock(memory) : NULL;
    char *result = NULL;
    if (wide && *wide) {
        int length =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
        if (length > 1) {
            result = xmalloc((size_t)length);
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, result, length, NULL,
                                NULL);
            if (out_len)
                *out_len = (size_t)length - 1;
        }
    }
    if (wide)
        GlobalUnlock(memory);
    CloseClipboard();
    return result;
}

char *clipboard_paste_image(size_t *out_len, long deadline_ms)
{
    (void)deadline_ms;
    static const wchar_t *const FORMATS[] = {L"PNG",        L"image/png", L"JFIF",
                                             L"image/jpeg", L"image/gif", L"image/webp"};
    if (!OpenClipboard(NULL))
        return NULL;
    char *result = NULL;
    for (size_t i = 0; !result && i < sizeof(FORMATS) / sizeof(*FORMATS); i++) {
        UINT format = RegisterClipboardFormatW(FORMATS[i]);
        HANDLE memory = GetClipboardData(format);
        if (!memory)
            continue;
        SIZE_T length = GlobalSize(memory);
        const void *bytes = GlobalLock(memory);
        if (bytes && length > 0 && length <= (64u << 20)) {
            result = xmalloc(length);
            memcpy(result, bytes, length);
            if (out_len)
                *out_len = length;
        }
        if (bytes)
            GlobalUnlock(memory);
    }
    CloseClipboard();
    return result;
}

#endif /* _WIN32 */
