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

static void test_reconcile_model(void)
{
    static const char BODY[] = "{\"data\": [{\"id\": \"served-a\"}, {\"id\": \"served-b\"}]}";
    char *adopt = (char *)"sentinel";

    /* Nothing configured → adopt the first served entry. */
    EXPECT(llamacpp_reconcile_model(BODY, NULL, &adopt) == 0);
    EXPECT_STR_EQ(adopt, "served-a");
    free(adopt);
    EXPECT(llamacpp_reconcile_model(BODY, "", &adopt) == 0);
    EXPECT_STR_EQ(adopt, "served-a");
    free(adopt);

    /* A configured model the server actually serves is kept — an explicit
     * --model / preset / state pick must not be replaced (even when it isn't
     * the first entry). */
    EXPECT(llamacpp_reconcile_model(BODY, "served-b", &adopt) == 0);
    EXPECT(adopt == NULL);

    /* A configured model absent from the live list is replaced (the server
     * answers with what it serves regardless); probe_model warns. */
    EXPECT(llamacpp_reconcile_model(BODY, "stale-model", &adopt) == 0);
    EXPECT_STR_EQ(adopt, "served-a");
    free(adopt);

    /* Unusable bodies / empty lists resolve nothing. */
    EXPECT(llamacpp_reconcile_model("{\"data\": []}", "m", &adopt) == -1);
    EXPECT(adopt == NULL);
    EXPECT(llamacpp_reconcile_model("not json", "m", &adopt) == -1);
    EXPECT(adopt == NULL);
}

int main(void)
{
    test_model_label();
    test_reconcile_model();
    T_REPORT();
}
