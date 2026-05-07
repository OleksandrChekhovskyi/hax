#!/usr/bin/env python3
"""Manual smoke test for streaming tool-output rendering.

Run via the bash tool inside hax (e.g. ask the model to invoke
``scripts/stream_demo.py <mode>``). Each mode exercises a different part of
the streaming pipeline.

Most modes take ``--delay <seconds>`` and ``--count <int>`` so a human can
watch the rendering at a comfortable pace. Defaults are tuned to the
specific behavior each mode is meant to surface.

Modes:
  short          A handful of lines with sleeps — proves live emission and
                 spinner-on-first-byte behavior.
  long           ~30 lines with sleeps — overruns the head budget so the
                 spinner-as-status takes over and finalize emits the
                 elision marker + tail block.
  ansi           Output with ANSI colors mixed in — verifies ctrl_strip
                 against streamed bytes (colors must NOT leak into the dim
                 block).
  burst          A fast burst of N lines with no sleep — exercises chunk
                 coalescing under throughput.
  slow           One line per 0.5s — useful for visually confirming the
                 status-line spinner repaints between commits.
  binary         A NUL byte followed by garbage — verifies the binary
                 guard suppresses the body and surfaces the marker.
  piped          Slow producer piped through ``grep`` — verifies that
                 grep stays line-buffered against an isatty stdout.
  python_buffer  Slow Python child with default buffering — verifies
                 Python's stdout stays line-buffered when isatty is true.
  fake-ninja     One progress line being rewritten in place via ``\\r`` —
                 the ninja/cargo pattern that motivated line_collapse.
  fake-meson     Committed test-result lines interleaved with an inline
                 rewriting progress header — what real meson test does.
  fake-vitest    Multi-row "window" at the bottom of the screen redrawn
                 with cursor-up + clear-line, periodically forwarding a
                 log line above the window. Vitest's reporter pattern;
                 deliberately the messiest case for our line-collapse
                 pipeline since cursor-up is stripped (we explicitly do
                 not support multi-line rewrites).
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time

# Restore default SIGPIPE handling so a downstream close (e.g.
# `… | head -n1`, or hax's PTY reader closing on output-cap) kills
# this process the way any other Unix tool would die — exit 141, no
# Python "Exception ignored while flushing sys.stdout" noise leaking
# into the bash tool's captured output. Python's default of ignoring
# SIGPIPE leaves later flushes to raise BrokenPipeError during
# interpreter shutdown, which the BrokenPipeError handler in main()
# can't always intercept.
try:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
except (AttributeError, ValueError):
    pass  # Windows or non-main thread — neither applies to our use.

ESC = "\033"
DIM = f"{ESC}[2m"
RESET = f"{ESC}[0m"
RED = f"{ESC}[31m"
BOLD_YELLOW = f"{ESC}[1;33m"
GREEN = f"{ESC}[1;32m"
CLEAR_EOL = f"{ESC}[K"
CURSOR_UP_1 = f"{ESC}[1A"


def write(s: str) -> None:
    """Write to stdout and flush immediately. The point of these demos
    is timing, so anything sitting in libc's line buffer past the
    intended emission moment defeats the test."""
    sys.stdout.write(s)
    sys.stdout.flush()


def sleep(seconds: float) -> None:
    if seconds > 0:
        time.sleep(seconds)


# ---------------- existing modes ----------------


def mode_short(args: argparse.Namespace) -> None:
    for i in range(1, args.count + 1):
        write(f"line {i}\n")
        sleep(args.delay)


def mode_long(args: argparse.Namespace) -> None:
    for i in range(1, args.count + 1):
        write(f"long line {i} — pretend this is build output of some kind\n")
        sleep(args.delay)


def mode_ansi(_: argparse.Namespace) -> None:
    write(f"{RED}red line{RESET}\n")
    write(f"{BOLD_YELLOW}bold yellow{RESET}\n")
    write(f"{DIM}dim line{RESET}\n")
    write("plain line\n")
    # OSC sequence with title-set + BEL terminator:
    write(f"{ESC}]0;some title\007after the OSC\n")


def mode_burst(args: argparse.Namespace) -> None:
    for i in range(1, args.count + 1):
        write(f"burst {i}\n")


def mode_slow(args: argparse.Namespace) -> None:
    for i in range(1, args.count + 1):
        write(f"slow {i}\n")
        sleep(args.delay)


def mode_binary(_: argparse.Namespace) -> None:
    sys.stdout.flush()
    fd = sys.stdout.fileno()
    os.write(fd, b"leading text\n")
    os.write(fd, b"\x00garbage\xff\xfe\x01\x02\n")
    os.write(fd, b"trailing text\n")


def mode_piped(args: argparse.Namespace) -> None:
    # Producer in a subshell, piped through grep. grep must stay line-
    # buffered against the TTY downstream — if it block-buffers, lines
    # only appear in chunks of a few KiB at a time.
    producer = (
        f"for i in $(seq 1 {args.count}); do "
        f"  echo \"filter me $i\"; "
        f"  sleep {args.delay}; "
        f"done"
    )
    subprocess.run(["sh", "-c", f"{producer} | grep filter"], check=False)


def mode_python_buffer(args: argparse.Namespace) -> None:
    # Run a Python child with the default buffering policy so we can
    # verify that on isatty stdout it stays line-buffered.
    code = (
        "import sys, time\n"
        f"for i in range({args.count}):\n"
        "    print(f'py line {i}')\n"
        f"    time.sleep({args.delay})\n"
    )
    subprocess.run([sys.executable, "-c", code], check=False)


# ---------------- new \r / windowed modes ----------------


def mode_fake_ninja(args: argparse.Namespace) -> None:
    """ninja / cargo / npm progress: a single line rewritten in place
    via \\r, then \\n to commit a final summary. line_collapse should
    fold the rewrites and only the final state survives in the head."""
    n = args.count
    targets = [
        "src/main.o", "src/util.o", "src/disp.o", "src/spinner.o",
        "src/tool_render.o", "src/line_collapse.o", "src/ctrl_strip.o",
        "src/utf8_sanitize.o", "src/agent.o", "src/turn.o", "src/sse.o",
        "src/http.o", "src/bg.o", "src/fs.o", "src/markdown.o",
        "src/diff.o", "src/trace.o", "src/transcript.o", "src/clipboard.o",
        "src/input.o",
    ]
    for i in range(1, n + 1):
        target = targets[(i - 1) % len(targets)]
        write(f"[{i}/{n}] CC {target}{CLEAR_EOL}\r")
        sleep(args.delay)
    # Final committed line (with \n) — real ninja prints this when done.
    write(f"[{n}/{n}] all targets up to date.\n")


def mode_fake_meson(args: argparse.Namespace) -> None:
    """meson test progress: a rewriting status header on its own line,
    interleaved with \\n-terminated result lines that scroll up as
    tests finish.

    Mirrors mesonbuild/mtest.py's ConsoleLogger:
      - Moon-phase spinner that advances every tick.
      - Elapsed seconds tick up while a test runs.
      - When tests run in parallel, the header round-robins the test
        name being shown every ROUND_ROBIN_TICKS — so the visible
        line keeps changing even if no test has finished yet.
      - The line is overprinted via ``\\x1b[K`` (emitted *before* the
        new content, ``\\r`` after) — same as meson's print_progress.
      - On test completion, the cleared progress slot is replaced by
        a ``\\n``-terminated result line and the next queued test
        joins the running pool."""
    spinner = ["🌑", "🌒", "🌓", "🌔", "🌕", "🌖", "🌗", "🌘"]
    # (name, verdict, ticks-to-finish). Spread the durations so the
    # round-robin and finish-order both produce visible motion.
    tests = [
        ("util", "OK", 4),
        ("tools/read", "OK", 3),
        ("interrupt", "OK", 2),
        ("sse", "OK", 6),
        ("turn", "OK", 3),
        ("bg", "OK", 5),
        ("ctrl_strip", "OK", 2),
        ("line_collapse", "OK", 4),
        ("utf8_sanitize", "OK", 3),
        ("disp", "OK", 2),
        ("cmd_classify", "OK", 1),
        ("transcript", "OK", 5),
        ("agent_core", "OK", 3),
        ("diff", "OK", 7),
        ("tool_render", "OK", 9),
        ("tools/edit", "OK", 6),
        ("tools/write", "OK", 8),
        ("env", "OK", 5),
        ("tools/bash", "OK", 10),
    ]
    PARALLELISM = 3
    ROUND_ROBIN_TICKS = 8  # ticks between header-test rotations
    LINE_WIDTH = 78
    n = len(tests)

    # Preamble matches a real `meson test` invocation backed by ninja.
    write("ninja: Entering directory `/tmp/build'\r\n")
    write("ninja: no work to do.\r\n")

    queue = [(idx + 1, name, verdict, dur) for idx, (name, verdict, dur) in enumerate(tests)]
    running: list[tuple[int, str, str, int]] = []
    elapsed: dict[int, int] = {}
    spinner_idx = 0
    rr_idx = 0
    rr_age = 0
    erase = ""  # mirrors meson's should_erase_line: empty on first emit, \x1b[K after

    while queue or running:
        # Refill the running pool from the queue.
        while len(running) < PARALLELISM and queue:
            t = queue.pop(0)
            running.append(t)
            elapsed[t[0]] = 0

        # Pick which running test the header shows; rotate every
        # ROUND_ROBIN_TICKS so a long-running pool still produces
        # visible motion in the line.
        if rr_age >= ROUND_ROBIN_TICKS:
            rr_idx = (rr_idx + 1) % len(running)
            rr_age = 0
        else:
            rr_idx %= len(running)
        cur_idx, cur_name, _, _ = running[rr_idx]
        cur_elapsed = elapsed[cur_idx]

        first_idx = running[0][0]
        last_idx = running[-1][0]
        count = f"{first_idx}/{n}" if len(running) == 1 else f"{first_idx}-{last_idx}/{n}"
        header = f"[{count}] {spinner[spinner_idx]} hax:{cur_name}"
        right = f"{cur_elapsed}/30s"
        spinner_idx = (spinner_idx + 1) % len(spinner)
        # Erase-then-write-then-CR: matches meson's print_progress order.
        gap = max(1, LINE_WIDTH - _visible_width(header) - len(right))
        write(f"{erase}{header}{' ' * gap}{right}\r")
        erase = CLEAR_EOL
        sleep(args.delay)
        rr_age += 1

        # Advance time on every running test and finish whichever ones
        # have hit their duration.
        for t in running:
            elapsed[t[0]] += 1
        finished_now = [t for t in running if elapsed[t[0]] >= t[3]]
        for t in finished_now:
            tid, tname, tverdict, _ = t
            running.remove(t)
            color = GREEN if tverdict == "OK" else RED
            took = elapsed[tid] * args.delay
            padded_name = f"{tname:<24}"
            # Erase the progress line, drop the result, and reset the
            # erase-state so the next progress emit starts cleanly.
            write(f"{erase}{tid:>2}/{n} hax:{padded_name}{color}{tverdict}{RESET}    {took:.2f}s\n")
            erase = ""
            # Round-robin slot may have shifted under us.
            if running and rr_idx >= len(running):
                rr_idx = 0

    # Final flush of the lingering erase code from the last progress
    # emit, just like meson's flush().
    if erase:
        write(erase)

    # Summary block. The blank lines around the totals are preserved
    # in the model's view (line_collapse keeps blanks) and dropped
    # from the live preview (bytes_are_blank skips them).
    write("\n")
    write(f"Ok:                {n}\n")
    write("Fail:              0\n\n")
    write("Full log written to /tmp/build/meson-logs/testlog.txt\n")


def _visible_width(s: str) -> int:
    """Approximate the on-screen width of an SGR-free string. Meson's
    moon-phase emoji each render two cells on most terminals; padding
    the progress line by codepoint count alone leaves the right-side
    duration text visibly shifted as the spinner advances. Counts
    every non-ASCII codepoint as 2 cells, which is good enough for
    moon-phase / box-drawing characters and overpadding wider glyphs
    is harmless (we only use the width to compute right-padding)."""
    w = 0
    for ch in s:
        w += 2 if ord(ch) >= 0x80 else 1
    return w


def mode_fake_vitest(args: argparse.Namespace) -> None:
    """vitest WindowRenderer pattern: an N-row window at the bottom
    redrawn periodically, with intercepted log lines forwarded above.

    The window is printed as ``rows.join('\\n')`` (no trailing \\n) and
    then cleared with ``\\033[K`` + N-1 × ``\\033[1A\\033[K``. After
    ctrl_strip drops those CSIs in our pipeline, the LAST row of one
    cycle concatenates with the FIRST row of the next — multi-line
    rewrites are deliberately out of scope for line_collapse, so this
    mode is here to confirm we degrade gracefully (no crash, no garbled
    spinner) rather than render perfectly."""
    files = [
        ("tests/preview.test.ts", 12),
        ("tests/render.test.ts", 8),
        ("tests/collapse.test.ts", 16),
        ("tests/strip.test.ts", 6),
    ]

    def render_window(progress: int, log: str | None = None) -> int:
        """Print one window snapshot, optionally preceded by a forwarded
        log line. Returns the window height (rows printed)."""
        rows: list[str] = []
        for path, total in files:
            done = min(progress, total)
            mark = "✓" if done == total else "⠋"
            rows.append(f" {mark} {path}  ({done}/{total})")
        rows.append("")
        done_total = sum(min(progress, t) for _, t in files)
        all_total = sum(t for _, t in files)
        rows.append(f" RUNNING  {done_total} of {all_total} tests")
        # vitest writes the log line first (above the window), then the
        # window content joined by \n with no trailing \n.
        if log:
            write(log)
            if not log.endswith("\n"):
                write("\n")
        write("\n".join(rows))
        return len(rows)

    def clear_window(height: int) -> None:
        # Clear current line, then move-up + clear for each row above.
        write(CLEAR_EOL)
        for _ in range(height - 1):
            write(f"{CURSOR_UP_1}{CLEAR_EOL}")

    height = render_window(0)
    sleep(args.delay)

    max_progress = max(t for _, t in files)
    log_at = {
        3: "stdout: warning — deprecated API used in foo.ts:42",
        7: "stderr: console.error → unhandled promise rejection",
    }
    for step in range(1, max_progress + 1):
        clear_window(height)
        height = render_window(step, log_at.get(step))
        sleep(args.delay)

    # Final summary: clear window, print completed file list and totals.
    clear_window(height)
    for path, total in files:
        write(f" ✓ {path}  ({total}/{total})\n")
    write("\n")
    write(f"Test Files  {len(files)} passed ({len(files)})\n")
    total_tests = sum(t for _, t in files)
    write(f"     Tests  {total_tests} passed ({total_tests})\n")
    write(f"  Duration  {args.delay * max_progress:.2f}s\n")


# ---------------- dispatch ----------------

# Per-mode defaults. Rewriting modes want a humanly-observable cadence;
# throughput modes default to no sleep.
MODE_DEFAULTS = {
    "short":         {"delay": 0.2,  "count": 5},
    "long":          {"delay": 0.1,  "count": 30},
    "ansi":          {"delay": 0.0,  "count": 0},
    "burst":         {"delay": 0.0,  "count": 200},
    "slow":          {"delay": 0.5,  "count": 20},
    "binary":        {"delay": 0.0,  "count": 0},
    "piped":         {"delay": 0.1,  "count": 40},
    "python_buffer": {"delay": 0.2,  "count": 15},
    "fake-ninja":    {"delay": 0.15, "count": 20},
    "fake-meson":    {"delay": 0.1,  "count": 0},
    "fake-vitest":   {"delay": 0.4,  "count": 0},
}

MODES = {
    "short":         mode_short,
    "long":          mode_long,
    "ansi":          mode_ansi,
    "burst":         mode_burst,
    "slow":          mode_slow,
    "binary":        mode_binary,
    "piped":         mode_piped,
    "python_buffer": mode_python_buffer,
    "fake-ninja":    mode_fake_ninja,
    "fake-meson":    mode_fake_meson,
    "fake-vitest":   mode_fake_vitest,
}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Streaming-output demo for hax's tool-render path.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("mode", choices=sorted(MODES.keys()),
                        help="Which streaming pattern to emit.")
    parser.add_argument("--delay", type=float, default=None,
                        help="Inter-line / inter-tick delay in seconds. "
                             "Default depends on mode.")
    parser.add_argument("--count", type=int, default=None,
                        help="Number of lines / iterations. Default depends "
                             "on mode; ignored by modes with fixed length.")
    args = parser.parse_args()

    defaults = MODE_DEFAULTS[args.mode]
    if args.delay is None:
        args.delay = defaults["delay"]
    if args.count is None:
        args.count = defaults["count"]

    try:
        MODES[args.mode](args)
    except BrokenPipeError:
        # Downstream (the agent's PTY reader) closed early — normal at
        # interrupt or output-cap. Don't dump a Python traceback into
        # what the model sees.
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
