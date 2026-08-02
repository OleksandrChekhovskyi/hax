#!/usr/bin/env python3
"""Emit streaming patterns for manual testing of hax's bash-tool renderer.

Run ``scripts/stream_demo.py --help`` for the available output patterns. Delay
and count defaults are chosen to make each pattern easy to observe in a live
hax session.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass

# Python ignores SIGPIPE by default, which can print shutdown noise after a reader exits.
signal.signal(signal.SIGPIPE, signal.SIG_DFL)

ESC = "\x1b"
DIM = f"{ESC}[2m"
RESET = f"{ESC}[0m"
RED = f"{ESC}[31m"
BOLD_YELLOW = f"{ESC}[1;33m"


@dataclass(frozen=True)
class DemoOptions:
    delay_seconds: float
    count: int
    prefix: str


@dataclass(frozen=True)
class Mode:
    emit: Callable[[DemoOptions], None]
    default_delay_seconds: float
    default_count: int
    default_prefix: str = ""


def write_stdout(text: str) -> None:
    sys.stdout.write(text)
    sys.stdout.flush()


def sleep_after_line(options: DemoOptions) -> None:
    if options.delay_seconds > 0:
        time.sleep(options.delay_seconds)


def run_allowing_sigpipe(command: list[str]) -> None:
    result = subprocess.run(command, check=False)
    if result.returncode not in (0, -signal.SIGPIPE, 128 + signal.SIGPIPE):
        result.check_returncode()


def emit_numbered_lines(options: DemoOptions) -> None:
    for line_number in range(1, options.count + 1):
        write_stdout(f"{options.prefix} {line_number}\n")
        sleep_after_line(options)


def emit_long_lines(options: DemoOptions) -> None:
    for line_number in range(1, options.count + 1):
        write_stdout(
            f"{options.prefix} {line_number} — pretend this is build output of some kind\n"
        )
        sleep_after_line(options)


def emit_ansi(_options: DemoOptions) -> None:
    write_stdout(f"{RED}red line{RESET}\n")
    write_stdout(f"{BOLD_YELLOW}bold yellow{RESET}\n")
    write_stdout(f"{DIM}dim line{RESET}\n")
    write_stdout("plain line\n")
    write_stdout(f"{ESC}]0;some title\x07after the OSC\n")


def emit_burst(options: DemoOptions) -> None:
    for line_number in range(1, options.count + 1):
        write_stdout(f"{options.prefix} {line_number}\n")


def emit_binary(_options: DemoOptions) -> None:
    sys.stdout.flush()
    stdout_fd = sys.stdout.fileno()
    os.write(stdout_fd, b"leading text\n")
    os.write(stdout_fd, b"\x00garbage\xff\xfe\x01\x02\n")
    os.write(stdout_fd, b"trailing text\n")


def emit_piped(options: DemoOptions) -> None:
    # grep needs explicit line buffering because its stdout is another pipe.
    producer = (
        f"for i in $(seq 1 {options.count}); do "
        f'echo "filter me $i"; sleep {options.delay_seconds}; '
        "done"
    )
    run_allowing_sigpipe(["sh", "-c", f"{producer} | grep --line-buffered filter"])


def emit_python_buffer(options: DemoOptions) -> None:
    # The bash tool sets PYTHONUNBUFFERED so this pipe receives each print promptly.
    child_code = (
        "import signal, time\n"
        "signal.signal(signal.SIGPIPE, signal.SIG_DFL)\n"
        f"for i in range({options.count}):\n"
        "    print(f'py line {i}')\n"
        f"    time.sleep({options.delay_seconds})\n"
    )
    run_allowing_sigpipe([sys.executable, "-c", child_code])


MODES = {
    "short": Mode(emit_numbered_lines, 0.2, 5, "line"),
    "long": Mode(emit_long_lines, 0.1, 30, "long line"),
    "ansi": Mode(emit_ansi, 0.0, 0),
    "burst": Mode(emit_burst, 0.0, 200, "burst"),
    "slow": Mode(emit_numbered_lines, 0.5, 20, "slow"),
    "binary": Mode(emit_binary, 0.0, 0),
    "piped": Mode(emit_piped, 0.1, 40),
    "python_buffer": Mode(emit_python_buffer, 0.2, 15),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=sorted(MODES))
    parser.add_argument(
        "--delay",
        type=float,
        help="inter-line delay in seconds (default depends on mode)",
    )
    parser.add_argument(
        "--count",
        type=int,
        help="number of lines or iterations (default depends on mode)",
    )
    parser.add_argument("--prefix", help="label for numbered output lines")
    args = parser.parse_args()
    if args.delay is not None and args.delay < 0:
        parser.error("--delay must be non-negative")
    if args.count is not None and args.count < 0:
        parser.error("--count must be non-negative")
    return args


def main() -> int:
    args = parse_args()
    mode = MODES[args.mode]
    options = DemoOptions(
        delay_seconds=(
            mode.default_delay_seconds if args.delay is None else args.delay
        ),
        count=mode.default_count if args.count is None else args.count,
        prefix=args.prefix or mode.default_prefix,
    )
    mode.emit(options)
    return 0


if __name__ == "__main__":
    sys.exit(main())
