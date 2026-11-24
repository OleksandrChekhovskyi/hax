/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "output.h"
#include "system/locale.h"
#include "terminal/focus.h"
#include "terminal/notify.h"

#define REPORT(pairs) "\x1b]7501;" pairs "\x1b\\"
#define WORKING       REPORT("state=working:app=hax")
#define DONE          REPORT("state=done:app=hax")
#define BEL           "\x07"
#define OSC9(body)    "\x1b]9;hax: " body "\x07"

static void expect_status(enum notify_state state, const char *want)
{
    char *written = NULL;
    size_t written_len = 0;
    FILE *out = open_memstream(&written, &written_len);
    EXPECT(out != NULL);
    notify_status_write(out, state);
    EXPECT(fclose(out) == 0);
    EXPECT_STR_EQ(written, want);
    free(written);
}

static void expect_refresh(const char *want)
{
    char *written = NULL;
    size_t written_len = 0;
    FILE *out = open_memstream(&written, &written_len);
    EXPECT(out != NULL);
    notify_status_refresh_write(out);
    EXPECT(fclose(out) == 0);
    EXPECT_STR_EQ(written, want);
    free(written);
}

static void expect_attention(const char *message, const char *want)
{
    char *written = NULL;
    size_t written_len = 0;
    FILE *out = open_memstream(&written, &written_len);
    EXPECT(out != NULL);
    notify_attention_write(out, message);
    EXPECT(fclose(out) == 0);
    EXPECT_STR_EQ(written, want);
    free(written);
}

/* Also starts each test without a terminal record, which the writer remembers, and with the
 * terminal reporting that it lost focus. */
static void use_terminal(const char *notify)
{
    focus_record(FOCUS_OUT);
    setenv("TERM", "xterm-256color", 1);
    unsetenv("TMUX");
    unsetenv("VTE_VERSION");
    unsetenv("HAX_PROGRAM_STATUS");
    if (notify)
        setenv("HAX_NOTIFY", notify, 1);
    else
        unsetenv("HAX_NOTIFY");
    expect_status(NOTIFY_STATE_CLEAR, REPORT("state=clear"));
}

static void test_defaults_report_status_and_ring(void)
{
    use_terminal(NULL);
    expect_status(NOTIFY_STATE_WORKING, WORKING);
    expect_status(NOTIFY_STATE_BLOCKED, REPORT("state=blocked:app=hax"));
    expect_status(NOTIFY_STATE_DONE, DONE);
    expect_status(NOTIFY_STATE_ERROR, REPORT("state=error:app=hax"));
    expect_status(NOTIFY_STATE_IDLE, REPORT("state=idle:app=hax"));
    expect_status(NOTIFY_STATE_CLEAR, REPORT("state=clear"));
    expect_attention("ignored by the bell", BEL);
}

static void test_refresh_applies_a_changed_setting(void)
{
    use_terminal(NULL);
    expect_status(NOTIFY_STATE_DONE, DONE);
    setenv("HAX_PROGRAM_STATUS", "off", 1);
    expect_refresh(REPORT("state=clear"));
    expect_refresh("");
    expect_status(NOTIFY_STATE_IDLE, "");
    unsetenv("HAX_PROGRAM_STATUS");
    expect_refresh(REPORT("state=idle:app=hax"));
}

static void withdraw_status(void *unused)
{
    (void)unused;
    notify_status_withdraw();
}

static void expect_withdraw(const char *want)
{
    char *written = t_capture_stdout(withdraw_status, NULL);
    EXPECT_STR_EQ(written, want);
    free(written);
}

static void test_withdraw_clears_only_a_reported_record(void)
{
    use_terminal(NULL);
    expect_withdraw("");
    expect_status(NOTIFY_STATE_DONE, DONE);
    expect_withdraw(REPORT("state=clear"));
}

static void test_notify_selects_only_the_attention_signal(void)
{
    use_terminal("bel");
    expect_attention(NULL, BEL);
    use_terminal("osc9");
    expect_attention(NULL, OSC9("ready"));
    use_terminal("growl");
    expect_attention(NULL, BEL);
    use_terminal("off");
    expect_attention(NULL, "");
    expect_status(NOTIFY_STATE_DONE, DONE);
}

static void test_program_status_selects_only_the_status(void)
{
    use_terminal(NULL);
    setenv("HAX_PROGRAM_STATUS", "off", 1);
    expect_status(NOTIFY_STATE_DONE, "");
    expect_attention(NULL, BEL);
}

static void test_osc9_shows_one_line_of_the_message(void)
{
    use_terminal("osc9");
    expect_attention(" \n ", OSC9("ready"));
    expect_attention("  First line\n\n  second\tline\x1b[31m\xc2\x9b ok ",
                     OSC9("First line second line [31m? ok"));
}

static void test_osc9_truncates_long_messages(void)
{
    use_terminal("osc9");
    char ascii[301];
    memset(ascii, 'a', 300);
    ascii[300] = '\0';
    char want_ascii[256];
    snprintf(want_ascii, sizeof(want_ascii), OSC9("%.*s\xe2\x80\xa6"), 197, ascii);
    expect_attention(ascii, want_ascii);

    /* A cut inside a two-byte character backs up to its first byte. */
    char accented[301];
    for (size_t i = 0; i < 300; i += 2)
        memcpy(accented + i, "\xc3\xa9", 2);
    accented[300] = '\0';
    char want_accented[256];
    snprintf(want_accented, sizeof(want_accented), OSC9("%.*s\xe2\x80\xa6"), 196, accented);
    expect_attention(accented, want_accented);
}

static void test_osc9_passes_through_tmux(void)
{
    use_terminal("osc9");
    setenv("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    expect_attention("done", "\x1bPtmux;\x1b" OSC9("done") "\x1b\\");
}

static void test_dumb_terminal_disables_only_auto(void)
{
    use_terminal(NULL);
    setenv("TERM", "dumb", 1);
    expect_status(NOTIFY_STATE_DONE, "");
    expect_attention(NULL, "");
    setenv("HAX_PROGRAM_STATUS", "on", 1);
    setenv("HAX_NOTIFY", "bel", 1);
    expect_status(NOTIFY_STATE_DONE, DONE);
    expect_attention(NULL, BEL);
}

static void test_old_vte_gets_only_the_bell(void)
{
    use_terminal(NULL);
    setenv("VTE_VERSION", "5202", 1);
    expect_status(NOTIFY_STATE_DONE, "");
    expect_attention(NULL, BEL);

    setenv("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    expect_status(NOTIFY_STATE_DONE, DONE);
    unsetenv("TMUX");

    setenv("VTE_VERSION", "7800", 1);
    expect_status(NOTIFY_STATE_DONE, DONE);
    setenv("VTE_VERSION", "5202", 1);
    setenv("HAX_PROGRAM_STATUS", "on", 1);
    expect_status(NOTIFY_STATE_DONE, DONE);
}

static void test_only_bel_rings_while_focused(void)
{
    use_terminal(NULL);
    focus_record(FOCUS_IN);
    expect_attention(NULL, "");
    use_terminal("osc9");
    focus_record(FOCUS_IN);
    expect_attention("done", "");
    use_terminal("bel");
    focus_record(FOCUS_IN);
    expect_attention(NULL, BEL);
}

int main(void)
{
    locale_init_utf8();
    test_defaults_report_status_and_ring();
    test_refresh_applies_a_changed_setting();
    test_withdraw_clears_only_a_reported_record();
    test_notify_selects_only_the_attention_signal();
    test_program_status_selects_only_the_status();
    test_osc9_shows_one_line_of_the_message();
    test_osc9_truncates_long_messages();
    test_osc9_passes_through_tmux();
    test_dumb_terminal_disables_only_auto();
    test_old_vte_gets_only_the_bell();
    test_only_bel_rings_while_focused();
    T_REPORT();
}
