/* SPDX-License-Identifier: MIT */
#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "xalloc.h"
#include "text/width.h"

char *t_capture_stdout(void (*body)(void *), void *user)
{
    fflush(stdout);
    int saved_fd = dup(STDOUT_FILENO);
    EXPECT(saved_fd >= 0);

    FILE *capture = tmpfile();
    EXPECT(capture != NULL);
    int capture_fd = fileno(capture);
    EXPECT(dup2(capture_fd, STDOUT_FILENO) >= 0);

    body(user);

    fflush(stdout);
    EXPECT(dup2(saved_fd, STDOUT_FILENO) >= 0);
    close(saved_fd);

    EXPECT(fseek(capture, 0, SEEK_END) == 0);
    long byte_count = ftell(capture);
    EXPECT(byte_count >= 0);
    EXPECT(fseek(capture, 0, SEEK_SET) == 0);
    char *output = xmalloc((size_t)byte_count + 1);
    size_t bytes_read = fread(output, 1, (size_t)byte_count, capture);
    output[bytes_read] = '\0';
    fclose(capture);
    return output;
}

char *t_strip_sgr(const char *text)
{
    char *out = xmalloc(strlen(text) + 1);
    size_t n = 0;
    while (*text) {
        if (*text == '\x1b' && text[1] == '[') {
            text += 2;
            while (*text && !(*text >= '@' && *text <= '~'))
                text++;
            if (*text)
                text++;
            continue;
        }
        out[n++] = *text++;
    }
    out[n] = '\0';
    return out;
}

void t_expect_rows_fit(const char *text, size_t max_cells)
{
    const char *row = text;
    while (*row) {
        const char *end = strchr(row, '\n');
        size_t row_bytes = end ? (size_t)(end - row) : strlen(row);
        char *copy = xasprintf("%.*s", (int)row_bytes, row);
        if (display_cells(copy) > max_cells)
            FAIL("row exceeds %zu cells: %s", max_cells, copy);
        free(copy);
        if (!end)
            break;
        row = end + 1;
    }
}
