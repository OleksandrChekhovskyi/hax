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
                 PYTHONUNBUFFERED in the bash env keeps stdout
                 line-flushed under our pipe (without it, CPython
                 block-buffers when stdout isn't a TTY and lines
                 only appear in 4 KiB chunks).
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time

# Restore default SIGPIPE handling so a downstream close (e.g.
# `… | head -n1`, or hax's pipe reader closing on output-cap) kills
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
    # Producer in a subshell, piped through grep. Under our pipe-based
    # bash, grep's stdout is also a pipe, so glibc/BSD grep block-buffer
    # by default — lines arrive in chunks. Pass `--line-buffered` to
    # force line flushes; this mode is here to surface the difference.
    producer = (
        f"for i in $(seq 1 {args.count}); do "
        f"  echo \"filter me $i\"; "
        f"  sleep {args.delay}; "
        f"done"
    )
    subprocess.run(["sh", "-c", f"{producer} | grep --line-buffered filter"], check=False)


def mode_python_buffer(args: argparse.Namespace) -> None:
    # Run a Python child with the default buffering policy. Python sees
    # stdout-not-a-tty under our pipe and would normally block-buffer
    # in 4 KiB chunks, but the bash tool sets PYTHONUNBUFFERED=1 in
    # the child env, so output flushes per print().
    code = (
        "import sys, time\n"
        f"for i in range({args.count}):\n"
        "    print(f'py line {i}')\n"
        f"    time.sleep({args.delay})\n"
    )
    subprocess.run([sys.executable, "-c", code], check=False)


# ---------------- dispatch ----------------

# Per-mode defaults. Slow modes default to a humanly-observable cadence;
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
        # Downstream (the agent's pipe reader) closed early — normal at
        # interrupt or output-cap. Don't dump a Python traceback into
        # what the model sees.
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
