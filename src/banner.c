/* SPDX-License-Identifier: MIT */
#include "banner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "config.h"
#include "provider.h"
#include "xalloc.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"
#include "terminal/width.h"
#include "text/width.h"

#define BANNER_GUTTER_CELLS 2
#define BANNER_INDENT_CELLS 4

static const char *banner_bar(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s▌%s", theme_open(THEME_CHROME), theme_close(THEME_CHROME));
    return buffer;
}

void banner_open(struct banner_writer *w, FILE *out)
{
    char bar[32];
    w->out = out;
    w->columns = display_width();
    w->col = BANNER_GUTTER_CELLS;
    w->fresh = 1;
    w->style = NULL;
    w->style_off = NULL;
    fprintf(out, "%s ", banner_bar(bar, sizeof(bar)));
}

/* Deduplicated transitions keep one SGR run across same-styled segments and separators. */
static void banner_style(struct banner_writer *w, const char *style_open, const char *style_off)
{
    if (w->style && style_open && strcmp(w->style, style_open) == 0)
        return;
    if (w->style)
        fputs(w->style_off, w->out);
    if (style_open)
        fputs(style_open, w->out);
    w->style = style_open;
    w->style_off = style_off;
}

static void banner_row_break(struct banner_writer *w)
{
    char bar[32];
    banner_style(w, NULL, NULL);
    fprintf(w->out, "\n%s   ", banner_bar(bar, sizeof(bar)));
    w->col = BANNER_INDENT_CELLS;
    w->fresh = 1;
}

void banner_put(struct banner_writer *w, const char *separator, const char *style_open,
                const char *style_off, const char *text)
{
    int cells = (int)display_cells(text);
    if (!w->fresh) {
        int sep_cells = (int)display_cells(separator);
        if (w->col + sep_cells + cells > w->columns) {
            banner_row_break(w);
        } else {
            banner_style(w, style_open, style_off);
            fputs(separator, w->out);
            w->col += sep_cells;
        }
    }
    banner_style(w, style_open, style_off);
    while (cells > w->columns - w->col) {
        size_t separator_bytes;
        size_t row_bytes = wrap_row_bytes(text, (size_t)(w->columns - w->col), &separator_bytes);
        fwrite(text, 1, row_bytes, w->out);
        text += row_bytes + separator_bytes;
        banner_row_break(w);
        banner_style(w, style_open, style_off);
        cells = (int)display_cells(text);
    }
    fputs(text, w->out);
    w->col += cells;
    w->fresh = 0;
}

void banner_close(struct banner_writer *w)
{
    banner_style(w, NULL, NULL);
    fputc('\n', w->out);
}

void banner_identity(FILE *out, const struct provider *provider,
                     const struct agent_session *session)
{
    struct banner_writer w;
    banner_open(&w, out);
    banner_put(&w, "", ANSI_BOLD, ANSI_BOLD_OFF, "hax");
    /* A preset may change the system prompt, so its stance must remain visible. */
    const char *preset = config_str("preset");
    if (preset && *preset) {
        char *stance = xasprintf("[%s]", preset);
        banner_put(&w, " ", theme_open(THEME_STANCE), theme_close(THEME_STANCE), stance);
        free(stance);
    }
    if (!provider) {
        banner_put(&w, " ", ANSI_DIM, ANSI_BOLD_OFF, "› no provider — use /provider");
        banner_close(&w);
        return;
    }
    char *head = xasprintf("› %s", provider->name ? provider->name : "?");
    banner_put(&w, " ", ANSI_DIM, ANSI_BOLD_OFF, head);
    free(head);
    const char *model_label = session->model_label ? session->model_label : session->model;
    char *tail;
    if (!session->model || !*session->model)
        tail = xstrdup("no model — use /model (or /provider)");
    else if (session->effort)
        tail = xasprintf("%s · %s", model_label, session->effort);
    else
        tail = xstrdup(model_label);
    banner_put(&w, " · ", ANSI_DIM, ANSI_BOLD_OFF, tail);
    free(tail);
    banner_close(&w);
}

void banner_print(const struct provider *provider, const struct agent_session *session)
{
    banner_identity(stdout, provider, session);
    struct banner_writer w;
    banner_open(&w, stdout);
    banner_put(&w, "", ANSI_DIM, ANSI_BOLD_OFF, "ctrl-d quit");
    banner_put(&w, " · ", ANSI_DIM, ANSI_BOLD_OFF, "try /help");
    banner_close(&w);
}
