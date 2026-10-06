/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>

#include "effort.h"
#include "harness.h"
#include "provider.h"
#include "providers/deepseek.h"

#define BALANCES_MAX 4

static void parse_entry(const char *json, struct model_info *info)
{
    json_t *entry = json_loads(json, 0, NULL);
    EXPECT(entry != NULL);
    model_info_init(info);
    deepseek_parse_model(entry, info);
    json_decref(entry);
}

static void test_live_model_entry(void)
{
    struct model_info info;
    parse_entry("{\"id\":\"deepseek-flash\",\"object\":\"model\",\"owned_by\":\"deepseek\","
                "\"name\":\"DeepSeek-V4.1-Flash\",\"context_window\":1048576,"
                "\"max_output_tokens\":393216,\"input_modalities\":[\"text\",\"image\"],"
                "\"output_modalities\":[\"text\"],"
                "\"effort\":{\"supported_levels\":[\"low\",\"high\",\"max\"],"
                "\"default_level\":\"high\"}}",
                &info);

    EXPECT(info.context == 1048576);
    EXPECT(info.max_output == 393216);
    EXPECT(info.image_input == PROVIDER_CAP_YES);
    EXPECT_STR_EQ(info.description, "DeepSeek-V4.1-Flash");
    EXPECT(info.efforts.known && info.efforts.count == 4);
    EXPECT(effort_set_has(&info.efforts, "none"));
    EXPECT(effort_set_has(&info.efforts, "max"));
    EXPECT(!effort_set_has(&info.efforts, "medium"));
    model_info_clear(&info);

    parse_entry("{\"id\":\"deepseek-v4-pro\",\"input_modalities\":[\"text\"]}", &info);
    EXPECT(info.image_input == PROVIDER_CAP_NO);
    model_info_clear(&info);
}

static void test_sparse_model_entry(void)
{
    struct model_info info;
    parse_entry("{\"id\":\"deepseek-next\",\"context_window\":-1,\"name\":\"\"}", &info);
    EXPECT(info.context == 0);
    EXPECT(info.max_output == 0);
    EXPECT(info.image_input == PROVIDER_CAP_UNKNOWN);
    EXPECT(!info.efforts.known);
    EXPECT(info.description == NULL);
    model_info_clear(&info);
}

static size_t parse_balance(const char *body, int *available, struct deepseek_balance *balances,
                            size_t max, json_t **root)
{
    *root = json_loads(body, 0, NULL);
    EXPECT(*root != NULL);
    return deepseek_balance_parse(*root, available, balances, max);
}

static void test_live_balance_shape(void)
{
    struct deepseek_balance balances[BALANCES_MAX];
    int available = 0;
    json_t *root;
    size_t n = parse_balance("{\"is_available\":true,\"balance_infos\":[{\"currency\":\"USD\","
                             "\"total_balance\":\"2.00\",\"granted_balance\":\"0.00\","
                             "\"topped_up_balance\":\"2.00\"}]}",
                             &available, balances, BALANCES_MAX, &root);

    EXPECT(n == 1);
    EXPECT(available == 1);
    EXPECT_STR_EQ(balances[0].currency, "USD");
    EXPECT_STR_EQ(balances[0].total, "2.00");
    EXPECT(balances[0].granted == NULL); /* zero grants are not worth a mention */
    json_decref(root);
}

static void test_malformed_balances_skipped(void)
{
    struct deepseek_balance balances[BALANCES_MAX];
    int available = 1;
    json_t *root;
    size_t n = parse_balance("{\"is_available\":false,\"balance_infos\":["
                             "{\"total_balance\":\"1.00\"},"
                             "{\"currency\":\"USD\",\"total_balance\":2},"
                             "{\"currency\":\"USD\",\"total_balance\":\"1.00\\u001b[2J\"},"
                             "{\"currency\":\"USD\",\"total_balance\":\"\\r1.00\"},"
                             "{\"currency\":\"CNY\",\"total_balance\":\"7.10\","
                             "\"granted_balance\":\"lots\"}]}",
                             &available, balances, BALANCES_MAX, &root);

    EXPECT(n == 1);
    EXPECT(available == 0);
    EXPECT_STR_EQ(balances[0].currency, "CNY");
    EXPECT(balances[0].granted == NULL);
    json_decref(root);
}

static void test_unexpected_balance_roots(void)
{
    struct deepseek_balance balances[BALANCES_MAX];
    int available = 0;
    json_t *root;

    EXPECT(parse_balance("{\"error\":{\"message\":\"nope\"}}", &available, balances, BALANCES_MAX,
                         &root) == 0);
    EXPECT(available == 1); /* unreported, not refused */
    json_decref(root);

    EXPECT(parse_balance("{\"balance_infos\":{}}", &available, balances, BALANCES_MAX, &root) == 0);
    json_decref(root);
}

static void test_balance_count_capped(void)
{
    struct deepseek_balance balances[1];
    int available = 0;
    json_t *root;
    size_t n = parse_balance("{\"balance_infos\":["
                             "{\"currency\":\"USD\",\"total_balance\":\"1.00\"},"
                             "{\"currency\":\"CNY\",\"total_balance\":\"7.10\"}]}",
                             &available, balances, 1, &root);
    EXPECT(n == 1);
    EXPECT_STR_EQ(balances[0].currency, "USD");
    json_decref(root);
}

static void expect_format(struct deepseek_balance balance, const char *want)
{
    char *text = deepseek_balance_format(&balance);
    EXPECT_STR_EQ(text, want);
    free(text);
}

static void test_balance_format(void)
{
    expect_format((struct deepseek_balance){.currency = "USD", .total = "5.00", .granted = "3.00"},
                  "$5.00 · $3.00 granted");
    expect_format((struct deepseek_balance){.currency = "CNY", .total = "7.10"}, "¥7.10");
    /* Other currencies keep their code, which is server text. */
    expect_format((struct deepseek_balance){.currency = "E\x1b[31mUR", .total = "1.00"},
                  "1.00 EUR");
}

int main(void)
{
    test_live_model_entry();
    test_sparse_model_entry();
    test_live_balance_shape();
    test_malformed_balances_skipped();
    test_unexpected_balance_roots();
    test_balance_count_capped();
    test_balance_format();
    T_REPORT();
}
