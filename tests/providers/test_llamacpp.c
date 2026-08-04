/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "providers/llamacpp.h"

static const char MODELS_RESPONSE[] =
    "{\"data\": [{\"id\": 7}, {}, {\"id\": \"served-a\"}, {\"id\": \"served-b\"}]}";

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

static void test_reconcile_unconfigured_model(void)
{
    char *replacement = (char *)"sentinel";
    EXPECT(llamacpp_reconcile_model(MODELS_RESPONSE, NULL, &replacement) == 0);
    EXPECT_STR_EQ(replacement, "served-a");
    free(replacement);

    EXPECT(llamacpp_reconcile_model(MODELS_RESPONSE, "", &replacement) == 0);
    EXPECT_STR_EQ(replacement, "served-a");
    free(replacement);
}

static void test_reconcile_served_model(void)
{
    char *replacement = (char *)"sentinel";
    EXPECT(llamacpp_reconcile_model(MODELS_RESPONSE, "served-b", &replacement) == 0);
    EXPECT(replacement == NULL);
}

static void test_reconcile_unserved_model(void)
{
    char *replacement = (char *)"sentinel";
    EXPECT(llamacpp_reconcile_model(MODELS_RESPONSE, "stale-model", &replacement) == 0);
    EXPECT_STR_EQ(replacement, "served-a");
    free(replacement);
}

static void test_reconcile_unusable_response(void)
{
    char *replacement = (char *)"sentinel";
    EXPECT(llamacpp_reconcile_model("{\"data\": []}", "model", &replacement) == -1);
    EXPECT(replacement == NULL);

    replacement = (char *)"sentinel";
    EXPECT(llamacpp_reconcile_model("not json", "model", &replacement) == -1);
    EXPECT(replacement == NULL);
}

static void test_unscoped_props_url(void)
{
    char *url = llamacpp_props_url("http://127.0.0.1:18080/v1", NULL);
    EXPECT_STR_EQ(url, "http://127.0.0.1:18080/props");
    free(url);

    url = llamacpp_props_url("http://127.0.0.1:18080/v1", "");
    EXPECT_STR_EQ(url, "http://127.0.0.1:18080/props");
    free(url);
}

static void test_model_scoped_props_url(void)
{
    char *url = llamacpp_props_url("http://127.0.0.1:18080/v1", "/models/Qwen 3.gguf");
    /* libcurl versions differ in the case of percent-escape hex digits. */
    EXPECT(strcmp(url, "http://127.0.0.1:18080/props?model=%2Fmodels%2FQwen+3.gguf") == 0 ||
           strcmp(url, "http://127.0.0.1:18080/props?model=%2fmodels%2fQwen+3.gguf") == 0);
    free(url);
}

int main(void)
{
    test_model_label();
    test_model_warning();
    test_reconcile_unconfigured_model();
    test_reconcile_served_model();
    test_reconcile_unserved_model();
    test_reconcile_unusable_response();
    test_unscoped_props_url();
    test_model_scoped_props_url();
    T_REPORT();
}
