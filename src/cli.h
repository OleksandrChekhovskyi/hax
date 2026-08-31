/* SPDX-License-Identifier: MIT */
#ifndef HAX_CLI_H
#define HAX_CLI_H

#include <stdio.h>

#include "agent_core.h"

enum cli_resume_mode {
    CLI_RESUME_NONE = 0,
    CLI_RESUME_LATEST,
    CLI_RESUME_SELECT,
};

enum cli_parse_result {
    CLI_PARSE_OK = 0,
    CLI_PARSE_EXIT,
    CLI_PARSE_ERROR,
};

enum cli_session_result {
    CLI_SESSION_READY = 0,
    CLI_SESSION_EXIT,
    CLI_SESSION_ERROR,
};

struct cli_selection {
    const char *provider;
    const char *model;
    const char *effort;
    const char *preset;
};

struct cli_options {
    struct hax_opts agent_options;
    struct cli_selection selection;
    enum cli_resume_mode resume_mode;
    const char *resume_id; /* borrowed argv value; NULL opens the resume picker */
    int first_prompt_arg;
    int one_shot;
    int bare;
    int no_session;
};

/* Parse and validate argv, initializing `options`. Stored option values borrow argv. */
enum cli_parse_result cli_parse(int argc, char **argv, struct cli_options *options);

/* Build the one-shot prompt from positional arguments or `input`. `input_is_tty` is supplied by
 * the caller so tests need no terminal. On success, `prompt` receives an owned string, or NULL
 * in interactive mode and on a promptless one-shot resume, which continues the recorded
 * conversation. */
int cli_read_prompt(const struct cli_options *options, int argc, char **argv, FILE *input,
                    int input_is_tty, char **prompt);

/* Resolve and mark active the requested session in the current directory. A selected path is
 * owned by the caller. */
enum cli_session_result cli_resolve_session(const struct cli_options *options, char **path);

/* Refuse malformed depths as well as values at the recursion cap. */
int cli_check_subagent_depth(void);

void cli_print_help(void);

#endif /* HAX_CLI_H */
