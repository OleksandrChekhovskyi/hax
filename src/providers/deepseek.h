/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_DEEPSEEK_H
#define HAX_PROVIDERS_DEEPSEEK_H

#include <jansson.h>
#include <stddef.h>

#include "provider.h"

/* DeepSeek API extras beyond the shared def: a /models listing that reports capabilities, and
 * the account balance behind /usage. */

/* Parse one /models entry into initialized `info`: context window, output cap, image input,
 * effort levels, and the versioned model name as its description. Unreported fields keep their
 * unknown values. */
void deepseek_parse_model(const json_t *entry, struct model_info *info);

/* One currency's balance; strings borrow from the parsed response. */
struct deepseek_balance {
    const char *currency;
    const char *total;
    const char *granted; /* promotional part of `total`; NULL when unreported or zero */
};

/* Fill up to `max` balances from a /user/balance response, one per entry with a currency and a
 * numeric total, in response order. `*available` receives whether the balance covers API calls,
 * 1 when unreported. Returns the count. */
size_t deepseek_balance_parse(json_t *root, int *available, struct deepseek_balance *balances,
                              size_t max);

/* The /usage value for one balance, e.g. "$5.00 · $3.00 granted". Owned. */
char *deepseek_balance_format(const struct deepseek_balance *balance);

/* /usage backend: the account balance. */
int deepseek_query_usage(struct provider *provider);

#endif /* HAX_PROVIDERS_DEEPSEEK_H */
