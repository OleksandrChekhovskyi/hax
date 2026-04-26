/* SPDX-License-Identifier: MIT */
#include "interrupt.h"

#include <string.h>

#include "harness.h"

/* Pure-logic tests for the Esc-vs-CSI byte classifier. The watcher's
 * threading and tty plumbing aren't exercised here — those are integration
 * concerns. We only verify the state machine that decides "is this byte
 * (or timeout) a confirmed bare Esc?". */

static int feed_str(struct interrupt_classifier *c, const char *s, size_t n)
{
    int fires = 0;
    for (size_t i = 0; i < n; i++)
        fires += interrupt_classifier_feed(c, (unsigned char)s[i]);
    return fires;
}

static void test_plain_bytes_never_fire(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    EXPECT(feed_str(&c, "hello world\n\t!@#", 16) == 0);
    EXPECT(c.state == IC_IDLE);
    /* Timeout in IDLE shouldn't fire either. */
    EXPECT(interrupt_classifier_timeout(&c) == 0);
}

static void test_bare_esc_fires_on_timeout(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    EXPECT(feed_str(&c, "\x1b", 1) == 0);
    EXPECT(c.state == IC_PENDING_ESC);
    EXPECT(interrupt_classifier_timeout(&c) == 1);
    EXPECT(c.state == IC_IDLE);
    /* Second timeout call (no pending) should not re-fire. */
    EXPECT(interrupt_classifier_timeout(&c) == 0);
}

static void test_csi_arrow_keys_ignored(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    /* \x1b[A is up-arrow; \x1b[B down; \x1b[C right; \x1b[D left. */
    EXPECT(feed_str(&c, "\x1b[A", 3) == 0);
    EXPECT(c.state == IC_IDLE);
    EXPECT(feed_str(&c, "\x1b[B\x1b[C\x1b[D", 9) == 0);
    EXPECT(c.state == IC_IDLE);
}

static void test_csi_with_params_ignored(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    /* \x1b[1;5C is Ctrl-Right on many terminals — params then final. */
    EXPECT(feed_str(&c, "\x1b[1;5C", 6) == 0);
    EXPECT(c.state == IC_IDLE);
    /* Mouse / DCS-style params (digits + semicolons) before final. */
    EXPECT(feed_str(&c, "\x1b[200~", 6) == 0);
    EXPECT(c.state == IC_IDLE);
}

static void test_ss3_function_keys_ignored(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    /* \x1bOP is F1 in many terminals; OQ=F2, OR=F3, OS=F4. */
    EXPECT(feed_str(&c, "\x1bOP", 3) == 0);
    EXPECT(c.state == IC_IDLE);
    EXPECT(feed_str(&c, "\x1bOQ\x1bOR\x1bOS", 9) == 0);
    EXPECT(c.state == IC_IDLE);
}

static void test_esc_followed_by_other_fires(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    /* Esc + 'a' — neither CSI nor SS3 prefix. Fires for the bare Esc;
     * 'a' is then a no-op in IDLE. Single fire total. The string is
     * split so '\x1b' doesn't greedily consume the following 'a'. */
    EXPECT(feed_str(&c,
                    "\x1b"
                    "a",
                    2) == 1);
    EXPECT(c.state == IC_IDLE);
}

static void test_esc_esc_fires_and_stays_pending(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    /* Two Escs in rapid succession: the second confirms the first is
     * bare → fire, then the second goes pending. A subsequent timeout
     * fires for it too. The user mashing Esc must not be swallowed. */
    EXPECT(feed_str(&c, "\x1b\x1b", 2) == 1);
    EXPECT(c.state == IC_PENDING_ESC);
    EXPECT(interrupt_classifier_timeout(&c) == 1);
    EXPECT(c.state == IC_IDLE);
}

static void test_esc_inside_idle_buffer(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    /* Plain text, then Esc — should still fire on timeout. */
    EXPECT(feed_str(&c, "abc\x1b", 4) == 0);
    EXPECT(c.state == IC_PENDING_ESC);
    EXPECT(interrupt_classifier_timeout(&c) == 1);
}

static void test_csi_truncated_resets(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    /* Defensive: a CSI cut short by a control byte returns to IDLE
     * without firing. (Real terminals don't emit this, but truncated
     * stdin during shutdown could.) */
    EXPECT(feed_str(&c, "\x1b[1\x07", 4) == 0);
    EXPECT(c.state == IC_IDLE);
}

static void test_init_clears_state(void)
{
    struct interrupt_classifier c;
    interrupt_classifier_init(&c);
    EXPECT(feed_str(&c, "\x1b", 1) == 0);
    EXPECT(c.state == IC_PENDING_ESC);
    interrupt_classifier_init(&c);
    EXPECT(c.state == IC_IDLE);
    /* Re-init must wipe the pending Esc — otherwise a stale state from
     * a previous arm window could fire spuriously on the next timeout. */
    EXPECT(interrupt_classifier_timeout(&c) == 0);
}

int main(void)
{
    test_plain_bytes_never_fire();
    test_bare_esc_fires_on_timeout();
    test_csi_arrow_keys_ignored();
    test_csi_with_params_ignored();
    test_ss3_function_keys_ignored();
    test_esc_followed_by_other_fires();
    test_esc_esc_fires_and_stays_pending();
    test_esc_inside_idle_buffer();
    test_csi_truncated_resets();
    test_init_clears_state();
    T_REPORT();
}
