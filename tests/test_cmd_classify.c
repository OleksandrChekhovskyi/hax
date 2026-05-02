/* SPDX-License-Identifier: MIT */
#include "cmd_classify.h"
#include "harness.h"

static void check_yes(const char *cmd)
{
    if (!cmd_is_exploration(cmd))
        FAIL("expected exploration: %s", cmd);
}

static void check_no(const char *cmd)
{
    if (cmd_is_exploration(cmd))
        FAIL("expected NOT exploration: %s", cmd);
}

int main(void)
{
    /* Plain read-only commands. */
    check_yes("ls");
    check_yes("ls -la");
    check_yes("ls /tmp");
    check_yes("pwd");
    check_yes("cat foo.c");
    check_yes("find . -name '*.c'");
    check_yes("find src -type f");
    check_yes("grep -r foo src");
    check_yes("rg pattern");
    check_yes("rg -n 'BUG|FIXME|TODO' src");
    check_yes("tree -L 2");
    check_yes("file /bin/sh");
    check_yes("stat foo.c");
    check_yes("which gcc");
    check_yes("git ls-files");
    check_yes("git grep TODO");
    check_yes("head foo.c");
    check_yes("tail -n 50 log.txt");
    check_yes("wc -l foo.c"); /* file operand → read-like */

    /* Pipelines that fold to exploration after stripping format filters. */
    check_yes("grep -r foo src | head -20");
    check_yes("rg pattern | head");
    check_yes("rg pattern | wc -l");
    check_yes("find . -name '*.c' | head -50");
    check_yes("find . -type f | sort | uniq");
    check_yes("ls /tmp | grep foo");
    check_yes("cat foo.c | head -100");
    check_yes("rg -l TODO | sort");
    check_yes("find . -type f | xargs grep foo");

    /* Connectors with cd/pushd prefix. */
    check_yes("cd /tmp && find . -name foo");
    check_yes("cd src && ls");
    check_yes("pushd /tmp && ls");
    check_yes("cd /tmp && grep -r foo .");
    check_yes("ls; pwd");
    check_yes("cd src && rg pattern | head -20");

    /* env-prefixed exploration. */
    check_yes("env LC_ALL=C grep foo bar.c");

    /* Stderr-merge tolerated. */
    check_yes("find . -name foo 2>/dev/null");
    check_yes("grep -r foo src 2>&1 | head");

    /* Action commands fall through. */
    check_no("git status");
    check_no("git log");
    check_no("rm foo");
    check_no("cargo build");
    check_no("cargo test");
    check_no("npm install");
    check_no("make");
    check_no("python script.py");
    check_no("./build.sh");

    /* Mixed pipelines: even one unknown segment disqualifies. */
    check_no("find . -name foo && rm bar");
    check_no("ls && cargo build");

    /* Mutating commands that look filter-shaped must NOT be exploration. */
    check_no("sed -i 's/x/y/' file.c");
    check_no("sed -i.bak 's/x/y/' file.c");
    check_no("sed --in-place 's/x/y/' file.c");
    check_no("sed --in-place=.bak 's/x/y/' file.c");
    /* Combined short flags: -i hidden inside a cluster like -Ei. */
    check_no("sed -Ei 's/x/y/' file.c");
    check_no("sed -Eni 's/x/y/' file.c");
    check_no("sed -Ei.bak 's/x/y/' file.c");
    check_no("cat foo | tee out.txt");
    check_no("tee out.txt");
    check_no("find . -name '*.tmp' | xargs rm");
    check_no("ls | xargs mv");

    /* find/fd mutating actions — even though the leader is read-like. */
    check_no("find . -delete");
    check_no("find . -name '*.tmp' -delete");
    check_no("find . -exec rm {} \\;");
    check_no("find . -name '*.c' -exec grep TODO {} +"); /* conservative */
    check_no("find . -execdir sh -c 'echo $0' {} \\;");
    check_no("find . -ok rm {} \\;");
    check_no("fd -x rm");
    check_no("fd --exec rm");

    /* find file-output actions (write to a named file, not stdout). */
    check_no("find . -fprint files.txt");
    check_no("find . -fprint0 files.txt");
    check_no("find . -name '*.c' -fprintf out.txt '%p\\n'");
    check_no("find . -fls listing.txt");
    /* Stdout-print actions stay exploration. */
    check_yes("find . -print");
    check_yes("find . -print0");
    check_yes("find . -name '*.c' -printf '%p\\n'");

    /* Format-filter writers: file operand looks read-like but a flag
     * turns the command into a writer. */
    check_no("sort -o out.txt in.txt");
    check_no("sort -oOUT in.txt"); /* combined short form */
    check_no("sort --output=out.txt in.txt");
    check_no("sort --output out.txt in.txt");
    check_no("sort -o out.txt"); /* writes via stdin */
    /* Sort without -o stays read-like with a file operand, or format-
     * only in a pipeline (pipeline accepts iff some other segment
     * supplies a real source). */
    check_yes("sort in.txt");
    check_yes("sort -n in.txt");
    check_no("sort | head"); /* both segments are format-only — no source */
    check_yes("rg foo | sort -u");

    /* Script-body side effects: cheap substring detection.
     * - awk's `system(` shell escape.
     * - sed write/execute commands at command boundaries (`w`/`W`/`e`
     *   followed by whitespace, preceded by start, `;`, `{`, or `}`).
     * Approximation only — script-body parsing is out of scope. */
    check_no("awk '{system(\"touch /tmp/x\")}' file.txt");
    check_no("awk 'BEGIN{system(\"rm -rf x\")}' file.txt");
    check_no("awk -e '{system(\"id\")}' -- file.txt");
    check_no("sed -n 'w out' file.txt");
    check_no("sed 'w /tmp/x' file.txt");
    check_no("sed -e 'w out' file.txt");
    check_no("sed '1{w out;}' file.txt");
    check_no("sed '1; w out' file.txt");
    check_no("sed -e '1,$ W out' file.txt");
    check_no("sed 'e cat /etc/passwd' file.txt"); /* GNU execute */
    check_no("sed 's/x/y/e' file.txt");           /* GNU eval flag at end of token */
    check_no("sed 's/foo//e' file.txt");          /* eval flag, empty replacement */
    check_no("sed 's/x/y/e;n' file.txt");         /* eval flag followed by `;` */
    /* Documented false negatives — accepted by design (see top of
     * cmd_classify.c). Pinned so a future "fix" that catches them is
     * a deliberate decision, not an accidental change. */
    check_yes("awk '{print > \"f\"}' file.txt");   /* in-script redirection */
    check_yes("awk '{print | \"cmd\"}' file.txt"); /* in-script pipe */
    check_yes("sed '1w out' file.txt");            /* digit-prefix address */
    check_yes("sed '2W out' file.txt");

    /* Read-only scripts with the same letters/words still classify. */
    check_yes("awk '{print system_var}' file.txt"); /* `system_var`, not `system(` */
    check_yes("awk '/systems/{print}' file.txt");
    check_yes("sed 's/word/WORD/' file.txt"); /* `w` inside regex content */
    check_yes("sed -n '1,20p' foo.c");        /* p is not a write cmd */
    check_yes("sed -e 's/a/b/g' foo.c");

    /* awk -i inplace mutates; plain awk reads. */
    check_no("awk -i inplace '{print}' file.txt");
    check_no("awk -iinplace '{print}' file.txt");     /* joined short-opt */
    check_yes("awk -i tools.awk '{print}' file.txt"); /* loads library, read-only */
    check_yes("awk -ifoo.awk '{print}' file.txt");    /* joined library load */
    check_yes("awk '{print}' file.txt");
    check_no("awk '{print $1}'"); /* format-only, no source — needs a real producer */
    check_yes("rg foo | awk '{print $2}'");

    /* tree -o writes its listing to a file. */
    check_no("tree -o out.txt");
    check_no("tree -oOUT");
    check_no("tree --output=out.txt");
    check_no("tree --output out.txt");
    check_no("tree -L 2 -o out.txt");
    /* tree without -o stays exploration. */
    check_yes("tree");
    check_yes("tree -L 2");
    check_yes("tree src");

    /* fd --exec= / --exec-batch= equals-form spellings. */
    check_no("fd --exec=rm");
    check_no("fd -e c --exec=rm");
    check_no("fd --exec-batch=rm");
    check_no("fd '*.tmp' --exec=rm");

    /* less/more `-o`/`-O`/`--log-file` writes input to a file. */
    check_no("less -o copy.txt input.txt");
    check_no("less -ocopy.txt input.txt");
    check_no("less -O copy.txt input.txt");
    check_no("less --log-file=copy.txt input.txt");
    check_no("less --log-file copy.txt input.txt");
    check_no("more -o copy.txt input.txt");
    /* less/more without these flags stay read. */
    check_yes("less foo.c");
    check_yes("more foo.c");

    /* Standalone `&` backgrounds the leader and runs the rest. */
    check_no("ls & rm file");
    check_no("grep foo bar.c & make");
    /* `&&` and `2>&1` must keep working. */
    check_yes("ls && find .");
    check_yes("grep -r foo src 2>&1 | head");

    /* xargs wrapping known-safe commands stays exploration. */
    check_yes("find . -type f | xargs grep foo");
    check_yes("find src -type f | xargs cat");

    /* `2>` boundary: a command name ending in `2` must not masquerade
     * as a stderr redirect. */
    check_no("python2 >/dev/null");
    check_no("foo2 >/tmp/log");

    /* Output redirection / substitution / heredoc all reject. */
    check_no("ls > out.txt");
    check_no("cat foo > bar");
    check_no("grep foo src >> log");
    check_no("cat <<EOF\nhi\nEOF");
    check_no("echo $(date)");
    check_no("ls `pwd`");
    check_no("diff <(ls a) <(ls b)");

    /* Double quotes still expand command substitution and backticks —
     * only single quotes truly suppress. */
    check_no("echo \"$(touch /tmp/x)\"");
    check_no("ls \"`touch /tmp/x`\"");
    check_no("cat \"foo $(date) bar\"");
    /* Single-quoted versions are literal text — safe. */
    check_yes("grep '$(touch /tmp/x)' file.c");
    check_yes("grep '`pwd`' file.c");
    /* Escaped `$` inside double quotes is literal, not substitution. */
    check_yes("grep \"\\$(notrun) literal\" file.c");

    /* Backslash is LITERAL inside single quotes per POSIX, so `'foo\'`
     * closes the quote at the second `'`. A previous bug treated `\'`
     * as an escape, kept the parser "inside" the quote, and let
     * mutating commands sneak past the separator scan. */
    check_no("grep 'foo\\' ; rm victim; echo 'bar' file");
    check_no("cat 'foo\\' ; rm bar");
    check_no("ls 'a\\' && rm /tmp/x");
    /* Standalone backslash inside single quotes is fine. */
    check_yes("grep 'foo\\bar' file.c");
    /* Inside double quotes, backslash escape still works — `\"` is a
     * literal `"` that does NOT close the quote. */
    check_yes("grep \"foo\\\"bar\" file.c");

    /* Empty / whitespace. */
    check_no("");
    check_no("   ");

    /* `cd` alone or `cd && cd` is a no-op cluster — treat as exploration
     * (model probably did this as a probe). */
    check_yes("cd /tmp");
    check_yes("cd /tmp && pwd");

    /* Quoted args don't trigger redirection detection. */
    check_yes("grep '>foo' bar.c");
    check_yes("rg \"a > b\" src");

    /* Sed/awk without file operand are pipeline format helpers. */
    check_yes("cat foo | sed -n '1,20p'");
    check_yes("rg foo | awk '{print $1}'");
    /* Sed with file operand — read-like. */
    check_yes("sed -n '1,20p' foo.c");

    /* Format-only commands need at least one real source segment.
     * Standalone filters (echo with no args, wc -l, head -20, sort -n,
     * rev, etc.) block on stdin or emit unrelated content; the user
     * should see them in full instead of a silent header. */
    check_no("echo");
    check_no("printf 'hi\\n'");
    check_no("wc -l");
    check_no("head -20");
    check_no("sort -n");
    check_no("rev");
    check_no("tr a b");
    /* `yes` is unbounded — never silent regardless of pairing. */
    check_no("yes");
    check_no("yes hello");
    check_no("yes | head -10");
    check_no("yes | grep foo");
    /* But a real producer + format filter still qualifies. */
    check_yes("cat foo.c | wc -l");
    check_yes("ls | head");
    check_yes("rg pattern | tail -n 5");

    /* Stdin-readers without a file operand block on stdin. The leader
     * matches read_cmd/search_cmd but argv carries no source, so they
     * downgrade to CC_FORMAT and reject when standalone. */
    check_no("cat");
    check_no("cat -n"); /* flags only, no file */
    check_no("less");
    check_no("more");
    check_no("nl");
    check_no("grep TODO"); /* pattern only — needs a file */
    check_no("grep -n TODO");
    check_no("egrep PATTERN");
    check_no("fgrep PATTERN");
    /* rg/ag/ack default to walking cwd, so pattern alone is enough. */
    check_yes("rg pattern");
    check_yes("ag pattern");
    check_yes("ack pattern");
    /* But the same in a downstream pipe is fine — stdin is the pipe. */
    check_yes("find . | cat");
    check_yes("ls /tmp | grep foo");

    /* Flag values must not be miscounted as file operands. `head -n 20`,
     * `sort -k 2`, `cut -d : -f 1` are all stdin-blocking. */
    check_no("head -n 20");
    check_no("head -c 100");
    check_no("tail -n 5");
    check_no("tail -c 50");
    check_no("sort -k 2");
    check_no("sort -t , -k 1");
    check_no("uniq -f 1");
    check_no("cut -d : -f 1");
    check_no("cut -f 1,2");
    check_no("paste -d ,");
    check_no("fold -w 80");
    /* Joined short-with-value (`-n5`, `-k2`) — value is in the cluster,
     * no extra token consumed. Same outcome: zero real operands. */
    check_no("head -n5");
    check_no("sort -k2");
    /* Same flags WITH a real file → read. */
    check_yes("head -n 20 foo.c");
    check_yes("sort -k 2 data.csv");
    check_yes("cut -d : -f 1 /etc/passwd");
    /* Same flags piped from a producer → filter, accepted. */
    check_yes("cat foo.c | head -n 5");
    check_yes("ls | cut -d / -f 1");
    check_yes("rg foo | sort -k 2");

    /* CC_FORMAT is only ignorable when fed by an upstream producer in
     * the same pipeline — `;`, `&&`, `||` open a fresh statement and
     * the format-filter loses its source. */
    check_no("ls; sort");
    check_no("ls; echo hi");
    check_no("ls; wc -l");
    check_no("ls && cat"); /* cat alone after && would hang */
    check_no("grep x file || printf 'no match\\n'");
    check_no("find . | sort; wc -l"); /* pipeline OK, then ;wc rejects */
    check_no("ls\nwc -l");            /* newline is statement-level */
    /* Empty segment between connectors must not "eat" the surrounding
     * separator context. `ls; | sort` is malformed shell, but the
     * classifier should still treat the `;` as a statement boundary
     * for `sort` rather than letting the empty middle pass through
     * the `|` and accept sort as a downstream filter. */
    check_no("ls; | sort");
    check_no("ls; ; sort");
    /* Multi-stage pipelines: filter chains without a fresh producer
     * propagate the producer state from segment to segment. */
    check_yes("cat foo.c | wc -l | head");
    check_yes("find . | sort | uniq | head");

    /* Newline is a top-level command separator — a multiline string
     * with a non-exploration command on a later line must reject. */
    check_no("cat foo\nrm bar");
    check_no("grep x file\nmake test");
    check_no("ls\ncargo build");
    check_no("ls\rcargo build"); /* lone CR also splits */
    check_no("ls\r\ncargo build");
    /* Multiple exploration commands across lines stay exploration. */
    check_yes("ls\npwd");
    check_yes("cat foo.c\ngrep TODO src");
    check_yes("ls\r\npwd"); /* CRLF line endings */
    /* Trailing/leading newlines and blank lines are no-ops. */
    check_yes("ls\n");
    check_yes("\nls");
    check_yes("ls\n\npwd");
    check_yes("cd /tmp\n");
    /* Newline inside quotes is part of the argument, not a separator. */
    check_yes("grep 'foo\nbar' file.c");

    /* Bare `<` input redirection from a file — read-only, so we
     * tolerate it. The parser doesn't separate redirect from operand,
     * but `<` ends up classified as a non-flag token which makes
     * format helpers like `wc` look read-like. Accidentally correct. */
    check_yes("wc -l < foo.c");

    T_REPORT();
}
