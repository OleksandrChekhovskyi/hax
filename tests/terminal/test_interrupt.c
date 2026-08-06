/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "terminal/interrupt.h"

static int feed(struct interrupt_classifier *classifier, const char *bytes)
{
    int escapes = 0;
    size_t length = strlen(bytes);
    for (size_t i = 0; i < length; i++)
        escapes += interrupt_classifier_feed(classifier, (unsigned char)bytes[i]);
    return escapes;
}

static void test_text_is_ignored(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);

    EXPECT(feed(&classifier, "hello world\n\t!@#") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
    EXPECT(interrupt_classifier_timeout(&classifier) == 0);
}

static void test_timeout_confirms_bare_escape(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);

    EXPECT(feed(&classifier, "\x1b") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_ESCAPE_PENDING);
    EXPECT(interrupt_classifier_timeout(&classifier) == 1);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
    EXPECT(interrupt_classifier_timeout(&classifier) == 0);
}

static void test_csi_arrow_keys_are_ignored(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);

    EXPECT(feed(&classifier, "\x1b[A") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
    EXPECT(feed(&classifier, "\x1b[B\x1b[C\x1b[D") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
}

static void test_csi_parameters_are_ignored(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);

    EXPECT(feed(&classifier, "\x1b[1;5C") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
    EXPECT(feed(&classifier, "\x1b[200~") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
}

static void test_ss3_function_keys_are_ignored(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);

    EXPECT(feed(&classifier, "\x1bOP") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
    EXPECT(feed(&classifier, "\x1bOQ\x1bOR\x1bOS") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
}

static void test_non_sequence_byte_confirms_escape(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);

    EXPECT(feed(&classifier, "\x1b"
                             "a") == 1);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
}

static void test_repeated_escape_confirms_each_press(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);

    EXPECT(feed(&classifier, "\x1b\x1b") == 1);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_ESCAPE_PENDING);
    EXPECT(interrupt_classifier_timeout(&classifier) == 1);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
}

static void test_escape_after_text_is_confirmed(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);

    EXPECT(feed(&classifier, "abc\x1b") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_ESCAPE_PENDING);
    EXPECT(interrupt_classifier_timeout(&classifier) == 1);
}

static void test_control_byte_ends_truncated_csi(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);

    EXPECT(feed(&classifier, "\x1b[1\x07") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
}

static void test_reset_discards_pending_escape(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);
    EXPECT(feed(&classifier, "\x1b") == 0);
    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_ESCAPE_PENDING);

    interrupt_classifier_init(&classifier);

    EXPECT(classifier.state == INTERRUPT_CLASSIFIER_IDLE);
    EXPECT(interrupt_classifier_timeout(&classifier) == 0);
}

int main(void)
{
    test_text_is_ignored();
    test_timeout_confirms_bare_escape();
    test_csi_arrow_keys_are_ignored();
    test_csi_parameters_are_ignored();
    test_ss3_function_keys_are_ignored();
    test_non_sequence_byte_confirms_escape();
    test_repeated_escape_confirms_each_press();
    test_escape_after_text_is_confirmed();
    test_control_byte_ends_truncated_csi();
    test_reset_discards_pending_escape();
    T_REPORT();
}
