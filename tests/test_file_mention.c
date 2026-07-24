/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "file_mention.h"
#include "system/path.h"
#include "terminal/input_core.h"
#include "util.h"

static void test_fzf_cmd_shape(void)
{
    char *cmd = file_mention_fzf_cmd("src");
    /* candidate sources: git first, pruned find as the non-repo fallback */
    EXPECT(strstr(cmd, "git ls-files -z --cached --others --exclude-standard") != NULL);
    EXPECT(strstr(cmd, "|| find .") != NULL);
    EXPECT(strstr(cmd, "-name .git") != NULL);
    /* NUL-delimited end-to-end: -z/-print0 producers, --read0/--print0
     * on fzf — line records would C-quote non-ASCII paths (git
     * core.quotePath) and can't carry newlines in filenames. */
    EXPECT(strstr(cmd, "-print0") != NULL);
    EXPECT(strstr(cmd, "--read0 --print0") != NULL);
    /* candidates are grouped so fzf sees both branches of the `||` */
    EXPECT(strncmp(cmd, "{ ", 2) == 0);
    EXPECT(strstr(cmd, "; } | fzf ") != NULL);
    EXPECT(strstr(cmd, "--query='src'") != NULL);
    /* no path prefix → no cd; walk stays cwd-relative */
    EXPECT(strstr(cmd, "cd ") == NULL);
    free(cmd);
}

static void test_fzf_cmd_query_quoting(void)
{
    /* shell metacharacters ride inside single quotes */
    char *cmd = file_mention_fzf_cmd("a b$(x)");
    EXPECT(strstr(cmd, "--query='a b$(x)'") != NULL);
    free(cmd);

    /* embedded single quote uses the '\'' splice */
    cmd = file_mention_fzf_cmd("it's");
    EXPECT(strstr(cmd, "--query='it'\\''s'") != NULL);
    free(cmd);

    /* empty and NULL queries still produce a well-formed flag */
    cmd = file_mention_fzf_cmd("");
    EXPECT(strstr(cmd, "--query=''") != NULL);
    free(cmd);
    cmd = file_mention_fzf_cmd(NULL);
    EXPECT(strstr(cmd, "--query=''") != NULL);
    free(cmd);
}

static void test_fzf_cmd_rooted(void)
{
    /* external tokens: cd to prefix-through-last-slash, filter = suffix */
    char *cmd = file_mention_fzf_cmd("../");
    EXPECT(strstr(cmd, "cd '../' 2>/dev/null") != NULL);
    EXPECT(strstr(cmd, "--query=''") != NULL);
    free(cmd);

    cmd = file_mention_fzf_cmd("../foo");
    EXPECT(strstr(cmd, "cd '../' 2>/dev/null") != NULL);
    EXPECT(strstr(cmd, "--query='foo'") != NULL);
    free(cmd);

    cmd = file_mention_fzf_cmd(".."); /* bare → trailing slash for rejoin */
    EXPECT(strstr(cmd, "cd '../' 2>/dev/null") != NULL);
    EXPECT(strstr(cmd, "--query=''") != NULL);
    free(cmd);

    cmd = file_mention_fzf_cmd("../../lib/x");
    EXPECT(strstr(cmd, "cd '../../lib/' 2>/dev/null") != NULL);
    EXPECT(strstr(cmd, "--query='x'") != NULL);
    free(cmd);

    cmd = file_mention_fzf_cmd("/tmp/x");
    EXPECT(strstr(cmd, "cd '/tmp/' 2>/dev/null") != NULL);
    EXPECT(strstr(cmd, "--query='x'") != NULL);
    free(cmd);

    /* ~/ → $HOME before quoting (mirror the builder) */
    char *expanded = expand_home("~/src/");
    char *quoted = shell_single_quote(expanded);
    char *want = xasprintf("cd %s 2>/dev/null", quoted);
    cmd = file_mention_fzf_cmd("~/src/fil");
    EXPECT(strstr(cmd, want) != NULL);
    EXPECT(strstr(cmd, "cd '~") == NULL);
    EXPECT(strstr(cmd, "--query='fil'") != NULL);
    free(cmd);
    free(want);
    free(quoted);
    free(expanded);

    cmd = file_mention_fzf_cmd("../a b$(x)");
    EXPECT(strstr(cmd, "cd '../' 2>/dev/null") != NULL);
    EXPECT(strstr(cmd, "--query='a b$(x)'") != NULL);
    free(cmd);
}

static void test_fzf_cmd_in_tree_keeps_cwd(void)
{
    /* in-tree/typos/./ / ~user stay cwd-relative with the full query */
    char *cmd = file_mention_fzf_cmd("src/tools/ba");
    EXPECT(strstr(cmd, "cd ") == NULL);
    EXPECT(strstr(cmd, "--query='src/tools/ba'") != NULL);
    free(cmd);

    cmd = file_mention_fzf_cmd("mispted/file");
    EXPECT(strstr(cmd, "cd ") == NULL);
    EXPECT(strstr(cmd, "--query='mispted/file'") != NULL);
    free(cmd);

    cmd = file_mention_fzf_cmd("./src/x");
    EXPECT(strstr(cmd, "cd ") == NULL);
    EXPECT(strstr(cmd, "--query='./src/x'") != NULL);
    free(cmd);

    cmd = file_mention_fzf_cmd("~other/x");
    EXPECT(strstr(cmd, "cd ") == NULL);
    EXPECT(strstr(cmd, "--query='~other/x'") != NULL);
    free(cmd);
}

/* The completer's pure match phase (its pick phase needs a tty + fzf,
 * so only the trigger policy is covered here). */
static int match(const char *buf, size_t len, size_t cursor, size_t *start, size_t *end)
{
    return file_mention_completer.match(buf, len, cursor, start, end, file_mention_completer.user);
}

static void test_match_triggers(void)
{
    size_t start = 999, end = 999;

    /* token at buffer start, cursor at end; span covers '@' → cursor */
    EXPECT(match("@foo", 4, 4, &start, &end) == 1);
    EXPECT(start == 0);
    EXPECT(end == 4);

    /* token after a space */
    EXPECT(match("see @src/m", 10, 10, &start, &end) == 1);
    EXPECT(start == 4);
    EXPECT(end == 10);

    /* token after a newline */
    EXPECT(match("x\n@foo", 6, 6, &start, &end) == 1);
    EXPECT(start == 2);

    /* cursor mid-token: span ends at the cursor, tail preserved */
    EXPECT(match("@src/m x", 8, 4, &start, &end) == 1);
    EXPECT(start == 0);
    EXPECT(end == 4);

    /* bare '@' with cursor right after: empty query still triggers */
    EXPECT(match("@", 1, 1, &start, &end) == 1);
    EXPECT(start == 0);
    EXPECT(end == 1);
}

static void test_match_rejects(void)
{
    size_t start, end;

    /* '@' mid-word (emails, decorators) doesn't start the token */
    EXPECT(match("foo@bar", 7, 7, &start, &end) == 0);

    /* cursor at or before the '@' */
    EXPECT(match("@foo", 4, 0, &start, &end) == 0);
    EXPECT(match("a @b", 4, 2, &start, &end) == 0);

    /* no '@' token under the cursor at all */
    EXPECT(match("hello", 5, 5, &start, &end) == 0);
    EXPECT(match("", 0, 0, &start, &end) == 0);
    EXPECT(match("@a c", 4, 4, &start, &end) == 0);

    /* out-of-range cursor */
    EXPECT(match("@a", 2, 3, &start, &end) == 0);
}

int main(void)
{
    test_fzf_cmd_shape();
    test_fzf_cmd_query_quoting();
    test_fzf_cmd_rooted();
    test_fzf_cmd_in_tree_keeps_cwd();
    test_match_triggers();
    test_match_rejects();
    T_REPORT();
}
