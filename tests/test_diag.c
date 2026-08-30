/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <unistd.h>

#include "diag.h"
#include "harness.h"

static void test_diag_sequence(void)
{
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    EXPECT(saved >= 0);
    FILE *tmp = tmpfile();
    EXPECT(tmp != NULL);
    EXPECT(dup2(fileno(tmp), STDERR_FILENO) >= 0);

    unsigned long before = hax_diag_sequence();
    hax_warn("sequence test");
    EXPECT(hax_diag_sequence() == before + 1);

    EXPECT(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    fclose(tmp);
}

int main(void)
{
    test_diag_sequence();

    T_REPORT();
}
