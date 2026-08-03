/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text/base64.h"

static void expect_encode(const char *in, const char *want)
{
    size_t n = 0;
    char *got = base64_encode(in, strlen(in), &n);
    EXPECT_STR_EQ(got, want);
    EXPECT(n == strlen(want));
    free(got);
}

static void expect_url_encode(const char *in, const char *want)
{
    size_t n = 0;
    char *got = base64url_encode(in, strlen(in), &n);
    EXPECT_STR_EQ(got, want);
    EXPECT(n == strlen(want));
    free(got);
}

static void expect_decode(const char *encoded, const void *want, size_t want_len)
{
    size_t decoded_len = 0;
    unsigned char *decoded = base64url_decode(encoded, strlen(encoded), &decoded_len);
    EXPECT(decoded != NULL);
    if (!decoded)
        return;
    EXPECT_MEM_EQ(decoded, decoded_len, want, want_len);
    EXPECT(decoded[decoded_len] == '\0');
    free(decoded);
}

static void expect_decode_rejected(const char *encoded)
{
    unsigned char *decoded = base64url_decode(encoded, strlen(encoded), NULL);
    EXPECT(decoded == NULL);
    free(decoded);
}

static void test_rfc4648_encode_vectors(void)
{
    expect_encode("", "");
    expect_encode("f", "Zg==");
    expect_encode("fo", "Zm8=");
    expect_encode("foo", "Zm9v");
    expect_encode("foob", "Zm9vYg==");
    expect_encode("fooba", "Zm9vYmE=");
    expect_encode("foobar", "Zm9vYmFy");
}

static void test_encode_binary(void)
{
    const unsigned char data[] = {0x00, 0xff, 0x10, 0x80, 0x00};
    char *got = base64_encode(data, sizeof(data), NULL);
    EXPECT_STR_EQ(got, "AP8QgAA=");
    free(got);
}

static void test_rfc4648_url_encode_vectors(void)
{
    expect_url_encode("", "");
    expect_url_encode("f", "Zg");
    expect_url_encode("fo", "Zm8");
    expect_url_encode("foo", "Zm9v");
    expect_url_encode("foob", "Zm9vYg");
    expect_url_encode("fooba", "Zm9vYmE");
    expect_url_encode("foobar", "Zm9vYmFy");
}

/* The two alphabets diverge only on the last two symbols, which is what makes JWT payloads safe. */
static void test_url_encode_uses_url_alphabet(void)
{
    const unsigned char data[] = {0xfb, 0xff, 0xbf};
    char *standard = base64_encode(data, sizeof(data), NULL);
    char *url = base64url_encode(data, sizeof(data), NULL);
    EXPECT_STR_EQ(standard, "+/+/");
    EXPECT_STR_EQ(url, "-_-_");
    free(standard);
    free(url);
}

static void test_url_encode_round_trips(void)
{
    const unsigned char data[] = {0x00, 0xff, 0x10, 0x80, 0x00};
    for (size_t len = 0; len <= sizeof(data); len++) {
        char *encoded = base64url_encode(data, len, NULL);
        size_t decoded_len = 0;
        unsigned char *decoded = base64url_decode(encoded, strlen(encoded), &decoded_len);
        EXPECT(decoded != NULL);
        if (decoded)
            EXPECT_MEM_EQ(decoded, decoded_len, data, len);
        free(decoded);
        free(encoded);
    }
}

static void test_base64url_padded_and_unpadded(void)
{
    expect_decode("", "", 0);
    expect_decode("Zg", "f", 1);
    expect_decode("Zg==", "f", 1);
    expect_decode("Zm8", "fo", 2);
    expect_decode("Zm8=", "fo", 2);
    expect_decode("Zm9v", "foo", 3);
}

static void test_base64url_binary(void)
{
    const unsigned char one_byte[] = {0xfb};
    const unsigned char three_bytes[] = {0xff, 0xff, 0x00};

    expect_decode("-w", one_byte, sizeof(one_byte));
    expect_decode("-w==", one_byte, sizeof(one_byte));
    expect_decode("__8A", three_bytes, sizeof(three_bytes));
}

static void test_base64url_rejects_bad_padding(void)
{
    expect_decode_rejected("=");
    expect_decode_rejected("Zg=");
    expect_decode_rejected("Zg===");
    expect_decode_rejected("Z=g=");
    expect_decode_rejected("Zm8==");
    expect_decode_rejected("Zg==x");
}

static void test_base64url_rejects_invalid_alphabet_and_length(void)
{
    expect_decode_rejected("+w==");
    expect_decode_rejected("/w==");
    expect_decode_rejected("Zg.=");
    expect_decode_rejected("A");
    expect_decode_rejected("AAAAA");
}

static void test_base64url_rejects_noncanonical_trailing_bits(void)
{
    expect_decode_rejected("Zh");
    expect_decode_rejected("Zh==");
    expect_decode_rejected("Zm9");
    expect_decode_rejected("Zm9=");
}

int main(void)
{
    test_rfc4648_encode_vectors();
    test_encode_binary();
    test_rfc4648_url_encode_vectors();
    test_url_encode_uses_url_alphabet();
    test_url_encode_round_trips();
    test_base64url_padded_and_unpadded();
    test_base64url_binary();
    test_base64url_rejects_bad_padding();
    test_base64url_rejects_invalid_alphabet_and_length();
    test_base64url_rejects_noncanonical_trailing_bits();
    T_REPORT();
}
