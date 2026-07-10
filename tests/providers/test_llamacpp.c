/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "providers/llamacpp.h"

static void expect_label(const char *model, const char *expected)
{
    char *label = llamacpp_model_label(NULL, model);
    EXPECT_STR_EQ(label, expected);
    free(label);
}

static void test_model_label(void)
{
    expect_label("/home/user/models/Qwen3.6-35B-A3B-UD-Q5_K_XL.gguf", "Qwen3.6-35B-A3B-UD-Q5_K_XL");
    expect_label("C:\\models\\Qwen.GGUF", "Qwen");
    expect_label("Qwen.gguf", "Qwen");
    expect_label("owner/model", "owner/model");
    expect_label("/models/model.bin", "/models/model.bin");
    expect_label(".gguf", ".gguf");
}

int main(void)
{
    test_model_label();
    T_REPORT();
}
