/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "text/placeholder.h"

static void expect_expanded(const char *text, const char *name, const char *value, const char *want)
{
    char *expanded = placeholder_expand(text, name, value);
    EXPECT_STR_EQ(expanded, want);
    free(expanded);
}

static void test_present(void)
{
    EXPECT(placeholder_present("http://127.0.0.1:{port}/v1", "port"));
    EXPECT(!placeholder_present("http://127.0.0.1:8080/v1", "port"));
    /* The braces are part of the token: a bare name or another placeholder does not count. */
    EXPECT(!placeholder_present("port {id}", "port"));
    EXPECT(!placeholder_present("", "port"));
}

static void test_expand(void)
{
    expect_expanded("http://127.0.0.1:{port}/v1", "port", "8080", "http://127.0.0.1:8080/v1");
    expect_expanded("{id}/{id}", "id", "s-1", "s-1/s-1");
    expect_expanded("{id}", "id", "", "");
    expect_expanded("plain", "id", "s-1", "plain");
    expect_expanded("{other} {id}", "id", "s-1", "{other} s-1");
    expect_expanded("", "id", "s-1", "");
}

int main(void)
{
    test_present();
    test_expand();
    T_REPORT();
}
