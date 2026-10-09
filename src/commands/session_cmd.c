/* SPDX-License-Identifier: MIT */
#include "commands/session_cmd.h"

#include <stdio.h>
#include <string.h>

#include "agent.h"
#include "agent_core.h"
#include "agent_stats.h"
#include "catalog.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "session.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "terminal/width.h"
#include "text/fmt.h"
#include "text/width.h"

#define SESSION_LABEL_WIDTH 14

/* Indent of value rows; also decides between the aligned label column and stacked layout. */
static int session_value_indent(int columns)
{
    int value_column = 2 + SESSION_LABEL_WIDTH;
    return columns - value_column >= UI_ROW_MIN_TEXT_CELLS ? value_column : UI_ROW_STACKED_INDENT;
}

/* Rows come in groups — identity, the live conversation, accounting — separated by a blank line
 * only when both sides printed something. */
struct session_rows {
    int printed_in_group;
    int separator_pending;
};

static void session_rows_group(struct session_rows *rows)
{
    rows->separator_pending = rows->printed_in_group;
    rows->printed_in_group = 0;
}

static void print_session_row(struct session_rows *rows, const char *label, const char *value)
{
    if (rows->separator_pending) {
        putchar('\n');
        rows->separator_pending = 0;
    }
    rows->printed_in_group = 1;
    ui_label_row(label, ANSI_DIM, value, ANSI_DIM, 2 + SESSION_LABEL_WIDTH, display_width());
}

/* Unknown and negligible cost estimates are omitted. The returned length may exceed the buffer. */
static int append_token_segment(char *row, size_t row_size, int row_length, const char *label,
                                long tokens, double cost)
{
    char formatted[32];
    if (row_length < 0 || (size_t)row_length >= row_size)
        return row_length;
    format_tokens(formatted, sizeof(formatted), tokens);
    row_length += snprintf(row + row_length, row_size - (size_t)row_length, "%s%s %s",
                           row_length ? " · " : "", label, formatted);
    if (cost >= COST_DISPLAY_MIN && row_length > 0 && (size_t)row_length < row_size) {
        format_cost(formatted, sizeof(formatted), cost);
        row_length += snprintf(row + row_length, row_size - (size_t)row_length, " ~%s", formatted);
    }
    return row_length;
}

/* Tokens by billing category, each with its rate estimate when known. */
static void format_usage_row(char *row, size_t row_size, const struct agent_stats_totals *usage)
{
    const struct catalog_split *split = usage->split_available ? &usage->split : NULL;
    int row_length = append_token_segment(row, row_size, 0, "in", usage->uncached_input_tokens,
                                          split ? split->cost_input : -1);
    if (usage->cached_tokens > 0)
        row_length = append_token_segment(row, row_size, row_length, "cache", usage->cached_tokens,
                                          split ? split->cost_cache_read : -1);
    if (usage->cache_write_tokens > 0)
        row_length =
            append_token_segment(row, row_size, row_length, "write", usage->cache_write_tokens,
                                 split ? split->cost_cache_write : -1);
    append_token_segment(row, row_size, row_length, "out", usage->output_tokens,
                         split ? split->cost_output : -1);
}

void session_command(struct agent_state *state)
{
    struct session_rows rows = {0};
    char row[256], formatted[32];

    const char *hint = session_log_resume_hint(state->session_log);
    print_session_row(&rows, "session", hint ? hint : "not recorded");

    const char *preset = config_str("preset");
    if (preset && *preset)
        print_session_row(&rows, "preset", preset);

    /* Report the effort the next request will carry after metadata resolution. */
    agent_session_resync_effort(state->session, state->provider, NULL);
    const char *provider_name =
        (state->provider && state->provider->name) ? state->provider->name : "?";
    const char *model = (state->session && state->session->model && *state->session->model)
                            ? state->session->model
                            : "?";
    const char *effort = state->session ? state->session->effort : NULL;
    if (effort && *effort)
        snprintf(row, sizeof(row), "%s · %s · %s", provider_name, model, effort);
    else
        snprintf(row, sizeof(row), "%s · %s", provider_name, model);
    /* When the identity overflows its row, break after the provider rather than between model
     * and effort; a hard newline in the value forces the row break. */
    int columns = display_width();
    if ((int)display_cells(row) > columns - session_value_indent(columns)) {
        if (effort && *effort)
            snprintf(row, sizeof(row), "%s\n%s · %s", provider_name, model, effort);
        else
            snprintf(row, sizeof(row), "%s\n%s", provider_name, model);
    }
    print_session_row(&rows, "provider", row);

    struct agent_stats stats;
    memset(&stats, 0, sizeof(stats));
    if (state->session)
        agent_stats_collect(state->session, 0, 0, state->provider, &stats);

    /* The live conversation. "User turn" throughout: a turn alone is a provider round-trip,
     * which is what requests counts below. */
    session_rows_group(&rows);
    if (stats.user_turns > 0 || stats.undone_user_turns > 0) {
        if (stats.undone_user_turns > 0)
            snprintf(row, sizeof(row), "%ld · %ld undone", stats.user_turns,
                     stats.undone_user_turns);
        else
            snprintf(row, sizeof(row), "%ld", stats.user_turns);
        print_session_row(&rows, "user turns", row);
    }

    if (stats.tool_calls > 0) {
        int row_length = snprintf(row, sizeof(row), "%ld", stats.tool_calls);
        for (size_t i = 0; i < AGENT_STATS_MAX_TOOLS && stats.tools[i].name; i++) {
            if (row_length < 0 || (size_t)row_length >= sizeof(row))
                break;
            row_length += snprintf(row + row_length, sizeof(row) - (size_t)row_length, " · %s %ld",
                                   stats.tools[i].name, stats.tools[i].count);
        }
        print_session_row(&rows, "tool calls", row);
    }

    /* Context is the latest request's window use. Until a request reports usage — a fresh
     * session, or a compaction or history cut invalidated the snapshot — usage is unknown
     * rather than zero, but the resolved window is still worth showing. */
    long window =
        model_meta_context(state->provider, state->session ? state->session->model : NULL);
    if (stats.context_tokens > 0) {
        format_context(row, sizeof(row), stats.context_tokens, window);
        print_session_row(&rows, "context", row);
    } else if (window > 0) {
        format_context(row, sizeof(row), -1, window);
        print_session_row(&rows, "context", row);
    }

    /* Accounting: everything the session did, undone user turns and retried requests included. */
    session_rows_group(&rows);
    if (stats.total.requests > 0) {
        snprintf(row, sizeof(row), "%ld", stats.total.requests);
        print_session_row(&rows, "requests", row);
    }

    if (stats.worked_ms > 0) {
        format_duration(formatted, sizeof(formatted), stats.worked_ms);
        print_session_row(&rows, "time worked", formatted);
    }

    /* Category costs are rate estimates even when the provider reported an exact total charge.
     * A conversation that switched models gets one row per model, since a single row would sum
     * tokens billed at different rates. */
    if (stats.total.input_tokens > 0 || stats.total.output_tokens > 0) {
        if (stats.n_models > 1) {
            /* Each row reads like a transcript footer: the model's spend, then its tokens. */
            for (size_t i = 0; i < stats.n_models; i++) {
                const struct agent_stats_model *entry = &stats.models[i];
                char tokens[200];
                format_usage_row(tokens, sizeof(tokens), &entry->totals);
                char spend[40] = "";
                if (entry->totals.spend > 0) {
                    format_cost(formatted, sizeof(formatted), entry->totals.spend);
                    snprintf(spend, sizeof(spend), "%s%s · ",
                             entry->totals.spend_estimated ? "~" : "", formatted);
                }
                snprintf(row, sizeof(row), "%s · %s\n%s%s", entry->provider ? entry->provider : "?",
                         entry->model ? entry->model : "?", spend, tokens);
                print_session_row(&rows, i == 0 ? "tokens" : "", row);
            }
        } else {
            format_usage_row(row, sizeof(row), &stats.total);
            print_session_row(&rows, "tokens", row);
        }
    }

    /* A mixed reported/estimated total remains an estimate. */
    if (stats.total.spend > 0) {
        format_cost(formatted, sizeof(formatted), stats.total.spend);
        snprintf(row, sizeof(row), "%s%s", stats.total.spend_estimated ? "~" : "", formatted);
        print_session_row(&rows, "spend", row);
    }

    /* Last, because it is the one cost the spend above does not include. */
    if (stats.inherited_user_turns > 0) {
        int row_length = snprintf(row, sizeof(row), "%ld user turn%s", stats.inherited_user_turns,
                                  stats.inherited_user_turns == 1 ? "" : "s");
        if (stats.inherited.spend > 0 && row_length > 0 && (size_t)row_length < sizeof(row)) {
            format_cost(formatted, sizeof(formatted), stats.inherited.spend);
            snprintf(row + row_length, sizeof(row) - (size_t)row_length, " · %s%s",
                     stats.inherited.spend_estimated ? "~" : "", formatted);
        }
        print_session_row(&rows, "inherited", row);
    }
}
