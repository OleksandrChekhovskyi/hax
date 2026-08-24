/* SPDX-License-Identifier: MIT */
#include <stddef.h>

#include "agent_core.h"
#include "agent_loop.h"
#include "harness.h"
#include "hax_embed.h"
#include "provider.h"
#include "util.h"

static int diag_calls;

static void count_diag(enum hax_diag_level level, const char *message, void *user)
{
    (void)level;
    (void)message;
    (void)user;
    diag_calls++;
}

static void test_init_is_not_reentrant(void)
{
    struct hax_embed_options options = {.diag = count_diag};
    EXPECT(hax_init(&options) == 0);

    diag_calls = 0;
    EXPECT(hax_init(&options) == -1);
    EXPECT(diag_calls == 1);

    hax_shutdown();
}

static void test_shutdown_is_idempotent_and_allows_reinit(void)
{
    struct hax_embed_options options = {0};
    EXPECT(hax_init(&options) == 0);
    hax_shutdown();
    hax_shutdown();
    EXPECT(hax_init(&options) == 0);
    hax_shutdown();
}

static void test_shutdown_without_init_is_a_no_op(void)
{
    hax_shutdown();
}

static void test_null_options_are_accepted(void)
{
    EXPECT(hax_init(NULL) == 0);
    hax_shutdown();
}

static void test_diag_sink_is_installed_and_removed(void)
{
    struct hax_embed_options options = {.diag = count_diag};
    EXPECT(hax_init(&options) == 0);
    diag_calls = 0;
    hax_warn("routed to the sink");
    EXPECT(diag_calls == 1);

    hax_shutdown();
    diag_calls = 0;
    hax_warn("back on stderr");
    EXPECT(diag_calls == 0);
}

static void test_abi_reports_this_build(void)
{
    const struct hax_abi *abi = hax_abi();
    EXPECT(abi != NULL);
    EXPECT(abi->version == HAX_ABI_VERSION);
    EXPECT(abi->sizeof_item == sizeof(struct item));
    EXPECT(abi->sizeof_agent_session == sizeof(struct agent_session));
    EXPECT(abi->sizeof_agent_loop_params == sizeof(struct agent_loop_params));
    EXPECT(abi->sizeof_agent_loop_result == sizeof(struct agent_loop_result));
    EXPECT(abi->sizeof_agent_loop_hooks == sizeof(struct agent_loop_hooks));
}

int main(void)
{
    test_shutdown_without_init_is_a_no_op();
    test_init_is_not_reentrant();
    test_shutdown_is_idempotent_and_allows_reinit();
    test_null_options_are_accepted();
    test_diag_sink_is_installed_and_removed();
    test_abi_reports_this_build();
    T_REPORT();
}
