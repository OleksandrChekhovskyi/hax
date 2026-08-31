/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "text/shell_quote.h"

static void test_shell_single_quote(void)
{
    char *q = shell_single_quote("plain");
    EXPECT_STR_EQ(q, "'plain'");
    free(q);

    q = shell_single_quote("it's");
    EXPECT_STR_EQ(q, "'it'\\''s'");
    free(q);

    /* metacharacters are inert inside single quotes — no escaping */
    q = shell_single_quote("a b;$(x)|&\"*");
    EXPECT_STR_EQ(q, "'a b;$(x)|&\"*'");
    free(q);

    q = shell_single_quote("");
    EXPECT_STR_EQ(q, "''");
    free(q);

    q = shell_single_quote(NULL);
    EXPECT_STR_EQ(q, "''");
    free(q);
}

int main(void)
{
    test_shell_single_quote();

    T_REPORT();
}
