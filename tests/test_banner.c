/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "banner.h"
#include "harness.h"
#include "provider.h"
#include "xalloc.h"
#include "system/locale.h"

/* Row layout is asserted on plain text; SGR runs vary with theme resolution. */
static char *strip_sgr(const char *s)
{
    char *out = xmalloc(strlen(s) + 1);
    size_t n = 0;
    while (*s) {
        if (*s == '\x1b' && s[1] == '[') {
            s += 2;
            while (*s && !(*s >= '@' && *s <= '~'))
                s++;
            if (*s)
                s++;
            continue;
        }
        out[n++] = *s++;
    }
    out[n] = '\0';
    return out;
}

static char *identity_rows(const struct provider *provider, const struct agent_session *session)
{
    char *raw = NULL;
    size_t raw_len = 0;
    FILE *stream = open_memstream(&raw, &raw_len);
    EXPECT(stream != NULL);
    banner_identity(stream, provider, session);
    fclose(stream);
    char *plain = strip_sgr(raw);
    free(raw);
    return plain;
}

static void test_identity_single_row(void)
{
    setenv("HAX_DISPLAY_WIDTH", "100", 1);
    struct provider provider = {.name = "mock"};
    struct agent_session session = {.model = (char *)"model-a", .effort = (char *)"high"};
    char *out = identity_rows(&provider, &session);
    EXPECT_STR_EQ(out, "▌ hax › mock · model-a · high\n");
    free(out);
}

static void test_identity_breaks_after_provider(void)
{
    /* Width 40 fits "hax › mock · abcdefghijklmnopqrstuv" (37 cells), so a greedy wrap would
     * strand the effort alone; the forced break keeps model and effort together instead. */
    setenv("HAX_DISPLAY_WIDTH", "40", 1);
    struct provider provider = {.name = "mock"};
    struct agent_session session = {.model = (char *)"abcdefghijklmnopqrstuv",
                                    .effort = (char *)"high"};
    char *out = identity_rows(&provider, &session);
    EXPECT_STR_EQ(out, "▌ hax › mock\n"
                       "▌   abcdefghijklmnopqrstuv · high\n");
    free(out);
}

static void test_identity_wraps_oversized_model(void)
{
    /* A model name wider than a whole row hard-breaks at the continuation budget (16 cells). */
    setenv("HAX_DISPLAY_WIDTH", "20", 1);
    struct provider provider = {.name = "mock"};
    struct agent_session session = {.model = (char *)"deepseek-ai/DeepSeek-R1-Distill-Llama-70B"};
    char *out = identity_rows(&provider, &session);
    EXPECT_STR_EQ(out, "▌ hax › mock\n"
                       "▌   deepseek-ai/Deep\n"
                       "▌   Seek-R1-Distill-\n"
                       "▌   Llama-70B\n");
    free(out);
}

static void test_identity_no_provider(void)
{
    setenv("HAX_DISPLAY_WIDTH", "100", 1);
    char *out = identity_rows(NULL, NULL);
    EXPECT_STR_EQ(out, "▌ hax › no provider — use /provider\n");
    free(out);
}

static void test_identity_no_model(void)
{
    setenv("HAX_DISPLAY_WIDTH", "100", 1);
    struct provider provider = {.name = "mock"};
    struct agent_session session = {0};
    char *out = identity_rows(&provider, &session);
    EXPECT_STR_EQ(out, "▌ hax › mock · no model — use /model (or /provider)\n");
    free(out);
}

static void test_identity_no_model_wraps_on_narrow_terminal(void)
{
    setenv("HAX_DISPLAY_WIDTH", "30", 1);
    struct provider provider = {.name = "mock"};
    struct agent_session session = {0};
    char *out = identity_rows(&provider, &session);
    EXPECT_STR_EQ(out, "▌ hax › mock\n"
                       "▌   no model — use /model (or\n"
                       "▌   /provider)\n");
    free(out);
}

static void test_identity_shows_preset_stance(void)
{
    setenv("HAX_DISPLAY_WIDTH", "100", 1);
    setenv("HAX_PRESET", "fast", 1);
    struct provider provider = {.name = "mock"};
    struct agent_session session = {.model = (char *)"model-a", .effort = (char *)"high"};
    char *out = identity_rows(&provider, &session);
    EXPECT_STR_EQ(out, "▌ hax [fast] › mock · model-a · high\n");
    free(out);
    unsetenv("HAX_PRESET");
}

static void test_identity_prefers_model_label(void)
{
    setenv("HAX_DISPLAY_WIDTH", "100", 1);
    struct provider provider = {.name = "mock"};
    struct agent_session session = {.model = (char *)"model-a", .model_label = (char *)"Model A"};
    char *out = identity_rows(&provider, &session);
    EXPECT_STR_EQ(out, "▌ hax › mock · Model A\n");
    free(out);
}

static void test_print_adds_key_tips(void)
{
    setenv("HAX_DISPLAY_WIDTH", "100", 1);
    struct provider provider = {.name = "mock"};
    struct agent_session session = {.model = (char *)"model-a"};

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    EXPECT(saved >= 0);
    FILE *tmp = tmpfile();
    EXPECT(tmp != NULL);
    EXPECT(dup2(fileno(tmp), STDOUT_FILENO) >= 0);

    banner_print(&provider, &session);

    fflush(stdout);
    EXPECT(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    EXPECT(fseek(tmp, 0, SEEK_SET) == 0);
    char raw[512];
    size_t got = fread(raw, 1, sizeof(raw) - 1, tmp);
    raw[got] = '\0';
    fclose(tmp);

    char *out = strip_sgr(raw);
    EXPECT_STR_EQ(out, "▌ hax › mock · model-a\n"
                       "▌ ctrl-d quit · try /help\n");
    free(out);
}

int main(void)
{
    /* Segment placement measures display cells of UTF-8 text. */
    locale_init_utf8();
    /* Both leak in from any hax parent process and would skew the fixtures below. */
    unsetenv("HAX_PRESET");
    unsetenv("HAX_DISPLAY_WIDTH");

    test_identity_single_row();
    test_identity_breaks_after_provider();
    test_identity_wraps_oversized_model();
    test_identity_no_provider();
    test_identity_no_model();
    test_identity_no_model_wraps_on_narrow_terminal();
    test_identity_shows_preset_stance();
    test_identity_prefers_model_label();
    test_print_adds_key_tips();

    T_REPORT();
}
