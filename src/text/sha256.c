/* SPDX-License-Identifier: MIT */
#include "text/sha256.h"

#include <stdint.h>
#include <string.h>

/* Implemented from the FIPS 180-4 specification; the round constants and initial hash values are
 * the standard's published tables (fractional parts of cube and square roots of the first
 * primes). */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32 - bits));
}

static void compress(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64];
    for (int t = 0; t < 16; t++) {
        w[t] = (uint32_t)block[4 * t] << 24 | (uint32_t)block[4 * t + 1] << 16 |
               (uint32_t)block[4 * t + 2] << 8 | (uint32_t)block[4 * t + 3];
    }
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int t = 0; t < 64; t++) {
        uint32_t sum1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t temp1 = h + sum1 + choose + K[t] + w[t];
        uint32_t sum0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256(const void *data, size_t len, unsigned char digest[SHA256_DIGEST_LEN])
{
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const unsigned char *cursor = data;
    size_t remaining = len;
    while (remaining >= 64) {
        compress(state, cursor);
        cursor += 64;
        remaining -= 64;
    }

    /* The padded tail is one block, or two when the 0x80 marker plus the 64-bit length do not fit
     * after the leftover bytes. */
    unsigned char tail[128];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, cursor, remaining);
    tail[remaining] = 0x80;
    size_t tail_len = remaining + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bit_len = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[tail_len - 1 - i] = (unsigned char)(bit_len >> (8 * i));
    compress(state, tail);
    if (tail_len == 128)
        compress(state, tail + 64);

    for (int i = 0; i < 8; i++) {
        digest[4 * i] = (unsigned char)(state[i] >> 24);
        digest[4 * i + 1] = (unsigned char)(state[i] >> 16);
        digest[4 * i + 2] = (unsigned char)(state[i] >> 8);
        digest[4 * i + 3] = (unsigned char)state[i];
    }
}
