/* SPDX-License-Identifier: MIT */
#include "system/locale.h"

#include <langinfo.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"

static int locale_is_utf8;
static int locale_children_are_utf8;

void locale_init_utf8(void)
{
    locale_is_utf8 = 0;
    locale_children_are_utf8 = 0;

    setlocale(LC_CTYPE, "");
    if (strcmp(nl_langinfo(CODESET), "UTF-8") == 0) {
        locale_is_utf8 = 1;
        locale_children_are_utf8 = 1;
        return;
    }
    /* OpenBSD ships no default locale at all, yet renders UTF-8 whatever the locale claims. This
     * process needs one regardless of the environment: mbrtowc() decodes the model's text here, and
     * without it every multibyte character is measured as its separate bytes.
     *
     * The spellings are PEP 538's, there being no portable one: glibc and the BSDs answer to
     * C.UTF-8, macOS only to a bare UTF-8. A language-bearing name is the last resort, for systems
     * where only specific locales were generated. */
    static const char *const CANDIDATES[] = {"C.UTF-8", "C.utf8", "UTF-8", "en_US.UTF-8"};
    const char *chosen = NULL;
    for (size_t i = 0; !chosen && i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++)
        chosen = setlocale(LC_CTYPE, CANDIDATES[i]);
    if (!chosen) {
        hax_warn("no UTF-8 locale found; non-ASCII text will be misread and misaligned");
        return;
    }
    locale_is_utf8 = 1;

    /* setlocale() reaches this process alone, so children need the choice published. A non-UTF-8
     * LC_ALL outranks LC_CTYPE and would mask it, and clearing it would move every other category
     * with it — leave the pinned locale alone and let callers render ASCII for children instead. */
    const char *lc_all = getenv("LC_ALL");
    if (lc_all && *lc_all)
        return;
    setenv("LC_CTYPE", chosen, 1);
    locale_children_are_utf8 = 1;
}

int locale_have_utf8(void)
{
    return locale_is_utf8;
}

const char *locale_child_ctype_override(void)
{
    if (locale_children_are_utf8 || !locale_is_utf8)
        return NULL;
    return setlocale(LC_CTYPE, NULL);
}
