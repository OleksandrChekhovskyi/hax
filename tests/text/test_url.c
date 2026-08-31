/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "harness.h"
#include "text/url.h"

static void expect_trimmed(const char *url, const char *want)
{
    char *trimmed = url_trim_trailing_slashes(url);
    EXPECT_STR_EQ(trimmed, want);
    free(trimmed);
}

static void test_trim_trailing_slashes(void)
{
    expect_trimmed("https://example.com/v1", "https://example.com/v1");
    expect_trimmed("https://example.com/v1///", "https://example.com/v1");
    expect_trimmed("/", "");
    expect_trimmed("", "");
}

static void expect_encoded(const char *value, const char *want)
{
    char *encoded = url_encode(value);
    EXPECT_STR_EQ(encoded, want);
    free(encoded);
}

static void test_encode(void)
{
    expect_encoded("", "");
    expect_encoded("AZaz09-._~", "AZaz09-._~");
    expect_encoded("a c/d&=?#", "a%20c%2Fd%26%3D%3F%23");
    /* A '+' is reserved, not a space, on the encoding side. */
    expect_encoded("a+b", "a%2Bb");
    expect_encoded("caf\xc3\xa9", "caf%C3%A9");

    struct buf out;
    buf_init(&out);
    buf_append_str(&out, "key=");
    url_encode_append(&out, "a b");
    char *appended = buf_steal(&out);
    EXPECT_STR_EQ(appended, "key=a%20b");
    free(appended);
}

static void expect_decoded(const char *encoded, const char *want)
{
    char *decoded = url_decode(encoded, strlen(encoded));
    EXPECT_STR_EQ(decoded, want);
    free(decoded);
}

static void test_decode(void)
{
    expect_decoded("", "");
    expect_decoded("plain", "plain");
    expect_decoded("a%20c%2Fd", "a c/d");
    expect_decoded("x+y", "x y");
    expect_decoded("%C3%A9", "\xc3\xa9");
    /* Truncated and invalid escapes pass through literally rather than failing. */
    expect_decoded("%", "%");
    expect_decoded("%4", "%4");
    expect_decoded("%zz", "%zz");
    expect_decoded("100%", "100%");

    /* `len` bounds the scan: a trailing escape outside it stays untouched. */
    char *decoded = url_decode("a%20b%20", 5);
    EXPECT_STR_EQ(decoded, "a b");
    free(decoded);
}

static void test_roundtrip(void)
{
    const char *value = "code+/=&? \xc3\xa9~end";
    char *encoded = url_encode(value);
    char *decoded = url_decode(encoded, strlen(encoded));
    EXPECT_STR_EQ(decoded, value);
    free(decoded);
    free(encoded);
}

int main(void)
{
    test_trim_trailing_slashes();
    test_encode();
    test_decode();
    test_roundtrip();
    T_REPORT();
}
