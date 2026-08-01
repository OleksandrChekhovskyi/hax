/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "tools/bash_classify.h"

static void expect_exploration(const char *command)
{
    if (!bash_command_is_exploration(command))
        FAIL("expected exploration: %s", command);
}

static void expect_not_exploration(const char *command)
{
    if (bash_command_is_exploration(command))
        FAIL("expected non-exploration: %s", command);
}

int main(void)
{
    /* Plain read-only commands. */
    expect_exploration("ls");
    expect_exploration("ls -la");
    expect_exploration("ls /tmp");
    expect_exploration("pwd");
    expect_exploration("cat foo.c");
    expect_exploration("find . -name '*.c'");
    expect_exploration("find src -type f");
    expect_exploration("grep -r foo src");
    expect_exploration("rg pattern");
    expect_exploration("rg -n 'BUG|FIXME|TODO' src");
    expect_exploration("tree -L 2");
    expect_exploration("file /bin/sh");
    expect_exploration("stat foo.c");
    expect_exploration("which gcc");
    expect_exploration("git ls-files");
    expect_exploration("git grep TODO");
    expect_exploration("head foo.c");
    expect_exploration("tail -n 50 log.txt");
    expect_exploration("wc -l foo.c"); /* file operand → read-like */

    /* Pipelines that fold to exploration after stripping format filters. */
    expect_exploration("grep -r foo src | head -20");
    expect_exploration("rg pattern | head");
    expect_exploration("rg pattern | wc -l");
    expect_exploration("find . -name '*.c' | head -50");
    expect_exploration("find . -type f | sort | uniq");
    expect_exploration("ls /tmp | grep foo");
    expect_exploration("cat foo.c | head -100");
    expect_exploration("rg -l TODO | sort");
    expect_exploration("find . -type f | xargs grep foo");

    /* Connectors with cd/pushd prefix. */
    expect_exploration("cd /tmp && find . -name foo");
    expect_exploration("cd src && ls");
    expect_exploration("pushd /tmp && ls");
    expect_exploration("cd /tmp && grep -r foo .");
    expect_exploration("ls; pwd");
    expect_exploration("cd src && rg pattern | head -20");

    /* env-prefixed exploration. */
    expect_exploration("env LC_ALL=C grep foo bar.c");

    /* Stderr-merge tolerated. */
    expect_exploration("find . -name foo 2>/dev/null");
    expect_exploration("grep -r foo src 2>&1 | head");

    /* Action commands fall through. */
    expect_not_exploration("git status");
    expect_not_exploration("git log");
    expect_not_exploration("rm foo");
    expect_not_exploration("cargo build");
    expect_not_exploration("cargo test");
    expect_not_exploration("npm install");
    expect_not_exploration("make");
    expect_not_exploration("python script.py");
    expect_not_exploration("./build.sh");

    /* Mixed pipelines: even one unknown segment disqualifies. */
    expect_not_exploration("find . -name foo && rm bar");
    expect_not_exploration("ls && cargo build");

    /* Mutating commands that look filter-shaped must NOT be exploration. */
    expect_not_exploration("sed -i 's/x/y/' file.c");
    expect_not_exploration("sed -i.bak 's/x/y/' file.c");
    expect_not_exploration("sed --in-place 's/x/y/' file.c");
    expect_not_exploration("sed --in-place=.bak 's/x/y/' file.c");
    /* Combined short flags: -i hidden inside a cluster like -Ei. */
    expect_not_exploration("sed -Ei 's/x/y/' file.c");
    expect_not_exploration("sed -Eni 's/x/y/' file.c");
    expect_not_exploration("sed -Ei.bak 's/x/y/' file.c");
    expect_not_exploration("cat foo | tee out.txt");
    expect_not_exploration("tee out.txt");
    expect_not_exploration("find . -name '*.tmp' | xargs rm");
    expect_not_exploration("ls | xargs mv");

    /* find/fd mutating actions — even though the leader is read-like. */
    expect_not_exploration("find . -delete");
    expect_not_exploration("find . -name '*.tmp' -delete");
    expect_not_exploration("find . -exec rm {} \\;");
    expect_not_exploration("find . -name '*.c' -exec grep TODO {} +"); /* conservative */
    expect_not_exploration("find . -execdir sh -c 'echo $0' {} \\;");
    expect_not_exploration("find . -ok rm {} \\;");
    expect_not_exploration("fd -x rm");
    expect_not_exploration("fd --exec rm");

    /* find file-output actions (write to a named file, not stdout). */
    expect_not_exploration("find . -fprint files.txt");
    expect_not_exploration("find . -fprint0 files.txt");
    expect_not_exploration("find . -name '*.c' -fprintf out.txt '%p\\n'");
    expect_not_exploration("find . -fls listing.txt");
    /* Stdout-print actions stay exploration. */
    expect_exploration("find . -print");
    expect_exploration("find . -print0");
    expect_exploration("find . -name '*.c' -printf '%p\\n'");

    /* Format-filter writers: file operand looks read-like but a flag
     * turns the command into a writer. */
    expect_not_exploration("sort -o out.txt in.txt");
    expect_not_exploration("sort -oOUT in.txt"); /* combined short form */
    expect_not_exploration("sort --output=out.txt in.txt");
    expect_not_exploration("sort --output out.txt in.txt");
    expect_not_exploration("sort -o out.txt"); /* writes via stdin */
    /* Sort without -o stays read-like with a file operand, or format-
     * only in a pipeline (pipeline accepts iff some other segment
     * supplies a real source). */
    expect_exploration("sort in.txt");
    expect_exploration("sort -n in.txt");
    expect_not_exploration("sort | head"); /* both segments are format-only — no source */
    expect_exploration("rg foo | sort -u");

    /* Script-body side effects: cheap substring detection.
     * - awk's `system(` shell escape.
     * - sed write/execute commands at command boundaries (`w`/`W`/`e`
     *   followed by whitespace, preceded by start, `;`, `{`, or `}`).
     * Approximation only — script-body parsing is out of scope. */
    expect_not_exploration("awk '{system(\"touch /tmp/x\")}' file.txt");
    expect_not_exploration("awk 'BEGIN{system(\"rm -rf x\")}' file.txt");
    expect_not_exploration("awk -e '{system(\"id\")}' -- file.txt");
    expect_not_exploration("sed -n 'w out' file.txt");
    expect_not_exploration("sed 'w /tmp/x' file.txt");
    expect_not_exploration("sed -e 'w out' file.txt");
    expect_not_exploration("sed '1{w out;}' file.txt");
    expect_not_exploration("sed '1; w out' file.txt");
    expect_not_exploration("sed -e '1,$ W out' file.txt");
    expect_not_exploration("sed 'e cat /etc/passwd' file.txt"); /* GNU execute */
    expect_not_exploration("sed 's/x/y/e' file.txt");           /* GNU eval flag at end of token */
    expect_not_exploration("sed 's/foo//e' file.txt");          /* eval flag, empty replacement */
    expect_not_exploration("sed 's/x/y/e;n' file.txt");         /* eval flag followed by `;` */
    /* Accepted false negatives define the intended boundary of the display heuristic. */
    expect_exploration("awk '{print > \"f\"}' file.txt");   /* in-script redirection */
    expect_exploration("awk '{print | \"cmd\"}' file.txt"); /* in-script pipe */
    expect_exploration("sed '1w out' file.txt");            /* digit-prefix address */
    expect_exploration("sed '2W out' file.txt");

    /* Read-only scripts with the same letters/words still classify. */
    expect_exploration("awk '{print system_var}' file.txt"); /* `system_var`, not `system(` */
    expect_exploration("awk '/systems/{print}' file.txt");
    expect_exploration("sed 's/word/WORD/' file.txt"); /* `w` inside regex content */
    expect_exploration("sed -n '1,20p' foo.c");        /* p is not a write cmd */
    expect_exploration("sed -e 's/a/b/g' foo.c");

    /* awk -i inplace mutates; plain awk reads. */
    expect_not_exploration("awk -i inplace '{print}' file.txt");
    expect_not_exploration("awk -iinplace '{print}' file.txt"); /* joined short-opt */
    expect_exploration("awk -i tools.awk '{print}' file.txt");  /* loads library, read-only */
    expect_exploration("awk -ifoo.awk '{print}' file.txt");     /* joined library load */
    expect_exploration("awk '{print}' file.txt");
    expect_not_exploration("awk '{print $1}'"); /* format-only, no source — needs a real producer */
    expect_exploration("rg foo | awk '{print $2}'");

    /* tree -o writes its listing to a file. */
    expect_not_exploration("tree -o out.txt");
    expect_not_exploration("tree -oOUT");
    expect_not_exploration("tree --output=out.txt");
    expect_not_exploration("tree --output out.txt");
    expect_not_exploration("tree -L 2 -o out.txt");
    /* tree without -o stays exploration. */
    expect_exploration("tree");
    expect_exploration("tree -L 2");
    expect_exploration("tree src");

    /* fd --exec= / --exec-batch= equals-form spellings. */
    expect_not_exploration("fd --exec=rm");
    expect_not_exploration("fd -e c --exec=rm");
    expect_not_exploration("fd --exec-batch=rm");
    expect_not_exploration("fd '*.tmp' --exec=rm");

    /* less/more `-o`/`-O`/`--log-file` writes input to a file. */
    expect_not_exploration("less -o copy.txt input.txt");
    expect_not_exploration("less -ocopy.txt input.txt");
    expect_not_exploration("less -O copy.txt input.txt");
    expect_not_exploration("less --log-file=copy.txt input.txt");
    expect_not_exploration("less --log-file copy.txt input.txt");
    expect_not_exploration("more -o copy.txt input.txt");
    /* less/more without these flags stay read. */
    expect_exploration("less foo.c");
    expect_exploration("more foo.c");

    /* Standalone `&` backgrounds the leader and runs the rest. */
    expect_not_exploration("ls & rm file");
    expect_not_exploration("grep foo bar.c & make");
    /* `&&` and `2>&1` must keep working. */
    expect_exploration("ls && find .");
    expect_exploration("grep -r foo src 2>&1 | head");

    /* xargs wrapping known-safe commands stays exploration. */
    expect_exploration("find . -type f | xargs grep foo");
    expect_exploration("find src -type f | xargs cat");

    /* `2>` boundary: a command name ending in `2` must not masquerade
     * as a stderr redirect. */
    expect_not_exploration("python2 >/dev/null");
    expect_not_exploration("foo2 >/tmp/log");

    /* Output redirection / substitution / heredoc all reject. */
    expect_not_exploration("ls > out.txt");
    expect_not_exploration("cat foo > bar");
    expect_not_exploration("grep foo src >> log");
    expect_not_exploration("cat <<EOF\nhi\nEOF");
    expect_not_exploration("echo $(date)");
    expect_not_exploration("ls `pwd`");
    expect_not_exploration("diff <(ls a) <(ls b)");

    /* Double quotes still expand command substitution and backticks —
     * only single quotes truly suppress. */
    expect_not_exploration("echo \"$(touch /tmp/x)\"");
    expect_not_exploration("ls \"`touch /tmp/x`\"");
    expect_not_exploration("cat \"foo $(date) bar\"");
    /* Single-quoted versions are literal text — safe. */
    expect_exploration("grep '$(touch /tmp/x)' file.c");
    expect_exploration("grep '`pwd`' file.c");
    /* Escaped `$` inside double quotes is literal, not substitution. */
    expect_exploration("grep \"\\$(notrun) literal\" file.c");

    /* Backslash is literal inside POSIX single quotes, so the second quote closes `'foo\'`. */
    expect_not_exploration("grep 'foo\\' ; rm victim; echo 'bar' file");
    expect_not_exploration("cat 'foo\\' ; rm bar");
    expect_not_exploration("ls 'a\\' && rm /tmp/x");
    /* Standalone backslash inside single quotes is fine. */
    expect_exploration("grep 'foo\\bar' file.c");
    /* Inside double quotes, backslash escape still works — `\"` is a
     * literal `"` that does NOT close the quote. */
    expect_exploration("grep \"foo\\\"bar\" file.c");

    /* Empty / whitespace. */
    expect_not_exploration("");
    expect_not_exploration("   ");

    /* `cd` alone or `cd && cd` is a no-op cluster — treat as exploration
     * (model probably did this as a probe). */
    expect_exploration("cd /tmp");
    expect_exploration("cd /tmp && pwd");

    /* Quoted args don't trigger redirection detection. */
    expect_exploration("grep '>foo' bar.c");
    expect_exploration("rg \"a > b\" src");

    /* Sed/awk without file operand are pipeline format helpers. */
    expect_exploration("cat foo | sed -n '1,20p'");
    expect_exploration("rg foo | awk '{print $1}'");
    /* Sed with file operand — read-like. */
    expect_exploration("sed -n '1,20p' foo.c");

    /* Format-only commands need at least one real source segment.
     * Standalone filters (echo with no args, wc -l, head -20, sort -n,
     * rev, etc.) block on stdin or emit unrelated content; the user
     * should see them in full instead of a silent header. */
    expect_not_exploration("echo");
    expect_not_exploration("printf 'hi\\n'");
    expect_not_exploration("wc -l");
    expect_not_exploration("head -20");
    expect_not_exploration("sort -n");
    expect_not_exploration("rev");
    expect_not_exploration("tr a b");
    /* `yes` is unbounded — never silent regardless of pairing. */
    expect_not_exploration("yes");
    expect_not_exploration("yes hello");
    expect_not_exploration("yes | head -10");
    expect_not_exploration("yes | grep foo");
    /* But a real producer + format filter still qualifies. */
    expect_exploration("cat foo.c | wc -l");
    expect_exploration("ls | head");
    expect_exploration("rg pattern | tail -n 5");

    /* Stdin readers without a source become filters and reject when standalone. */
    expect_not_exploration("cat");
    expect_not_exploration("cat -n"); /* flags only, no file */
    expect_not_exploration("less");
    expect_not_exploration("more");
    expect_not_exploration("nl");
    expect_not_exploration("grep TODO"); /* pattern only — needs a file */
    expect_not_exploration("grep -n TODO");
    expect_not_exploration("egrep PATTERN");
    expect_not_exploration("fgrep PATTERN");
    /* rg/ag/ack default to walking cwd, so pattern alone is enough. */
    expect_exploration("rg pattern");
    expect_exploration("ag pattern");
    expect_exploration("ack pattern");
    /* But the same in a downstream pipe is fine — stdin is the pipe. */
    expect_exploration("find . | cat");
    expect_exploration("ls /tmp | grep foo");

    /* Flag values must not be miscounted as file operands. `head -n 20`,
     * `sort -k 2`, `cut -d : -f 1` are all stdin-blocking. */
    expect_not_exploration("head -n 20");
    expect_not_exploration("head -c 100");
    expect_not_exploration("tail -n 5");
    expect_not_exploration("tail -c 50");
    expect_not_exploration("sort -k 2");
    expect_not_exploration("sort -t , -k 1");
    expect_not_exploration("uniq -f 1");
    expect_not_exploration("cut -d : -f 1");
    expect_not_exploration("cut -f 1,2");
    expect_not_exploration("paste -d ,");
    expect_not_exploration("fold -w 80");
    /* Joined short-with-value (`-n5`, `-k2`) — value is in the cluster,
     * no extra token consumed. Same outcome: zero real operands. */
    expect_not_exploration("head -n5");
    expect_not_exploration("sort -k2");
    /* Same flags WITH a real file → read. */
    expect_exploration("head -n 20 foo.c");
    expect_exploration("sort -k 2 data.csv");
    expect_exploration("cut -d : -f 1 /etc/passwd");
    /* Same flags piped from a producer → filter, accepted. */
    expect_exploration("cat foo.c | head -n 5");
    expect_exploration("ls | cut -d / -f 1");
    expect_exploration("rg foo | sort -k 2");

    /* Filters require an upstream producer in the same pipeline; statement separators reset it. */
    expect_not_exploration("ls; sort");
    expect_not_exploration("ls; echo hi");
    expect_not_exploration("ls; wc -l");
    expect_not_exploration("ls && cat"); /* cat alone after && would hang */
    expect_not_exploration("grep x file || printf 'no match\\n'");
    expect_not_exploration("find . | sort; wc -l"); /* pipeline OK, then ;wc rejects */
    expect_not_exploration("ls\nwc -l");            /* newline is statement-level */
    /* Empty segment between connectors must not "eat" the surrounding
     * separator context. `ls; | sort` is malformed shell, but the
     * classifier should still treat the `;` as a statement boundary
     * for `sort` rather than letting the empty middle pass through
     * the `|` and accept sort as a downstream filter. */
    expect_not_exploration("ls; | sort");
    expect_not_exploration("ls; ; sort");
    /* Multi-stage pipelines: filter chains without a fresh producer
     * propagate the producer state from segment to segment. */
    expect_exploration("cat foo.c | wc -l | head");
    expect_exploration("find . | sort | uniq | head");

    /* Newline is a top-level command separator — a multiline string
     * with a non-exploration command on a later line must reject. */
    expect_not_exploration("cat foo\nrm bar");
    expect_not_exploration("grep x file\nmake test");
    expect_not_exploration("ls\ncargo build");
    expect_not_exploration("ls\rcargo build"); /* lone CR also splits */
    expect_not_exploration("ls\r\ncargo build");
    /* Multiple exploration commands across lines stay exploration. */
    expect_exploration("ls\npwd");
    expect_exploration("cat foo.c\ngrep TODO src");
    expect_exploration("ls\r\npwd"); /* CRLF line endings */
    /* Trailing/leading newlines and blank lines are no-ops. */
    expect_exploration("ls\n");
    expect_exploration("\nls");
    expect_exploration("ls\n\npwd");
    expect_exploration("cd /tmp\n");
    /* Newline inside quotes is part of the argument, not a separator. */
    expect_exploration("grep 'foo\nbar' file.c");

    /* Bare `<` input redirection from a file — read-only, so we
     * tolerate it. The parser doesn't separate redirect from operand,
     * but `<` ends up classified as a non-flag token which makes
     * format helpers like `wc` look read-like. Accidentally correct. */
    expect_exploration("wc -l < foo.c");

    T_REPORT();
}
