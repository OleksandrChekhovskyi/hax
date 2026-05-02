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
    /* Sort without -o stays read-like / format. */
    check_yes("sort in.txt");
    check_yes("sort -n in.txt");
    check_yes("sort | head");
    check_yes("rg foo | sort -u");

    /* awk -i inplace mutates; plain awk reads. */
    check_no("awk -i inplace '{print}' file.txt");
    check_no("awk -iinplace '{print}' file.txt");     /* joined short-opt */
    check_yes("awk -i tools.awk '{print}' file.txt"); /* loads library, read-only */
    check_yes("awk -ifoo.awk '{print}' file.txt");    /* joined library load */
    check_yes("awk '{print}' file.txt");
    check_yes("awk '{print $1}'"); /* no operand → format */
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
    check_yes("echo '$(touch /tmp/x)'");
    check_yes("grep '`pwd`' file.c");
    /* Escaped `$` inside double quotes is literal, not substitution. */
    check_yes("echo \"\\$(notrun) literal\"");

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

    /* Bare `<` input redirection from a file — read-only, so we
     * tolerate it. The parser doesn't separate redirect from operand,
     * but `<` ends up classified as a non-flag token which makes
     * format helpers like `wc` look read-like. Accidentally correct. */
    check_yes("wc -l < foo.c");

    T_REPORT();
}
