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

static void test_rfc4648_vectors(void)
{
    expect_encode("", "");
    expect_encode("f", "Zg==");
    expect_encode("fo", "Zm8=");
    expect_encode("foo", "Zm9v");
    expect_encode("foob", "Zm9vYg==");
    expect_encode("fooba", "Zm9vYmE=");
    expect_encode("foobar", "Zm9vYmFy");
}

static void test_binary(void)
{
    /* NULs and high bytes — the payloads images are made of. */
    const unsigned char data[] = {0x00, 0xff, 0x10, 0x80, 0x00};
    char *got = base64_encode(data, sizeof(data), NULL);
    EXPECT_STR_EQ(got, "AP8QgAA=");
    free(got);
}

int main(void)
{
    test_rfc4648_vectors();
    test_binary();
    T_REPORT();
}
