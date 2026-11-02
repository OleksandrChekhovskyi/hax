/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>

#include "cred_store.h"
#include "harness.h"
#include "commands/login.h"
#include "text/completion.h"

static void test_login_offers_every_flow(void)
{
    struct completion choices = {0};
    login_choices(&choices);
    EXPECT(choices.count == 2);
    if (choices.count == 2) {
        EXPECT_STR_EQ(choices.candidates[0], "codex");
        EXPECT_STR_EQ(choices.candidates[1], "kimi");
    }
    completion_free(&choices);
}

static void test_logout_offers_held_logins(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    struct completion choices = {0};
    logout_choices(&choices);
    EXPECT(choices.count == 0);

    json_t *entry = json_pack("{s:s, s:s, s:s}", "access_token", "at", "refresh_token", "rt",
                              "account_id", "acc");
    EXPECT(cred_store_set("codex", entry) == 0);
    json_decref(entry);
    logout_choices(&choices);
    EXPECT(choices.count == 1);
    if (choices.count == 1)
        EXPECT_STR_EQ(choices.candidates[0], "codex");
    completion_free(&choices);

    entry = json_pack("{s:s,s:s,s:s,s:s,s:i}", "access_token", "at", "refresh_token", "rt",
                      "device_id", "d", "login_id", "l", "expires_at", 1234);
    json_object_set_new(entry, "generation", json_integer(0));
    EXPECT(cred_store_set("kimi", entry) == 0);
    json_decref(entry);
    logout_choices(&choices);
    EXPECT(choices.count == 2);
    if (choices.count == 2) {
        EXPECT_STR_EQ(choices.candidates[0], "codex");
        EXPECT_STR_EQ(choices.candidates[1], "kimi");
    }
    completion_free(&choices);
    EXPECT(cred_store_delete("kimi") == 1);
    logout_choices(&choices);
    EXPECT(choices.count == 1);
    completion_free(&choices);
}

int main(void)
{
    test_login_offers_every_flow();
    test_logout_offers_held_logins();
    T_REPORT();
}
