/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text/sha256.h"

static void expect_digest(const void *data, size_t len, const char *want_hex)
{
    unsigned char digest[SHA256_DIGEST_LEN];
    sha256(data, len, digest);
    char got_hex[2 * SHA256_DIGEST_LEN + 1];
    for (size_t i = 0; i < SHA256_DIGEST_LEN; i++)
        snprintf(got_hex + 2 * i, 3, "%02x", digest[i]);
    EXPECT_STR_EQ(got_hex, want_hex);
}

/* NIST FIPS 180-4 example vectors plus the SHA-256 test-suite million-'a' message. */
static void test_nist_vectors(void)
{
    expect_digest("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect_digest("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    char *million = malloc(1000000);
    EXPECT(million != NULL);
    if (million) {
        memset(million, 'a', 1000000);
        expect_digest(million, 1000000,
                      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        free(million);
    }
}

/* Lengths around the one-vs-two-block padding boundary all pad correctly. */
static void test_padding_boundaries(void)
{
    /* python3 -c 'import hashlib; print(hashlib.sha256(b"5" * n).hexdigest())' */
    expect_digest("5555555555555555555555555555555555555555555555555555555", 55,
                  "6ec1b49d80adafbe80682031f37aec4dc2ea60bc8c4be11103f08a57c6c9901c");
    expect_digest("55555555555555555555555555555555555555555555555555555555", 56,
                  "26d58665fbc81b1863cf357ba36e1305fe533a44557e85efe26e89d3e7036a94");
    expect_digest("5555555555555555555555555555555555555555555555555555555555555555", 64,
                  "911878b1ff94a6daecaba1d7ef6fda0b97116a16291389427b402a013a2ae79f");
}

int main(void)
{
    test_nist_vectors();
    test_padding_boundaries();
    T_REPORT();
}
