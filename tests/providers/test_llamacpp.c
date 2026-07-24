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

static void test_model_warning(void)
{
    char *warning = llamacpp_model_warning("codex/gpt-5.6-sol/high",
                                           "/home/user/models/Qwen3.6-35B-A3B-UD-Q5_K_XL.gguf");
    EXPECT_STR_EQ(warning, "llama.cpp: model 'codex/gpt-5.6-sol/high' is not served — using "
                           "'Qwen3.6-35B-A3B-UD-Q5_K_XL'");
    free(warning);

    warning = llamacpp_model_warning("/old/Qwen.gguf", "/new/Qwen.gguf");
    EXPECT_STR_EQ(warning, "llama.cpp: configured model is not served — using 'Qwen'");
    free(warning);
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

static void test_props_url(void)
{
    /* No model → bare /props on the same scheme/host/port, /v1 dropped. */
    char *u = llamacpp_props_url("http://127.0.0.1:18080/v1", NULL);
    EXPECT_STR_EQ(u, "http://127.0.0.1:18080/props");
    free(u);
    u = llamacpp_props_url("http://127.0.0.1:18080/v1", "");
    EXPECT_STR_EQ(u, "http://127.0.0.1:18080/props");
    free(u);

    /* A gguf path model id is URL-encoded into the query: slashes → %2F,
     * spaces → + (query form-encoding), so the probe reaches a valid,
     * model-scoped URL. Percent-escape hex digits may vary in case by
     * libcurl version. */
    u = llamacpp_props_url("http://127.0.0.1:18080/v1", "/models/Qwen 3.gguf");
    EXPECT(strcmp(u, "http://127.0.0.1:18080/props?model=%2Fmodels%2FQwen+3.gguf") == 0 ||
           strcmp(u, "http://127.0.0.1:18080/props?model=%2fmodels%2fQwen+3.gguf") == 0);
    free(u);
}

int main(void)
{
    test_model_label();
    test_model_warning();
    test_reconcile_model();
    test_props_url();
    T_REPORT();
}
