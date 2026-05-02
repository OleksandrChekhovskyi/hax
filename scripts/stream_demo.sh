#!/bin/sh
# Manual smoke test for streaming tool-output rendering.
#
# Run via the bash tool inside hax (e.g. ask the model to invoke
# `scripts/stream_demo.sh <mode>`). Each mode exercises a different part of
# the streaming pipeline:
#
#   short    A handful of lines with sleeps — proves live emission and
#            spinner-on-first-byte behavior.
#   long     ~30 lines with sleeps — overruns the head budget so the
#            spinner resumes and the elision + tail block render at the end.
#   ansi     Output with ANSI colors mixed in — verifies ctrl_strip works
#            against streamed bytes (colors must NOT leak into the dim block).
#   burst    A fast burst of 200 lines with no sleep — exercises chunk
#            coalescing under throughput.
#   slow     One line per 0.5s for ~10s — useful for visually confirming
#            the spinner re-shows after head fills.
#   binary   Emits a NUL byte followed by garbage — verifies the binary
#            guard suppresses the body and surfaces the marker.
set -e

mode="${1:-short}"

case "$mode" in
short)
    for i in 1 2 3 4 5; do
        echo "line $i"
        sleep 0.2
    done
    ;;
long)
    for i in $(seq 1 30); do
        echo "long line $i — pretend this is build output of some kind"
        sleep 0.1
    done
    ;;
ansi)
    printf '\033[31mred line\033[0m\n'
    printf '\033[1;33mbold yellow\033[0m\n'
    printf '\033[2mdim line\033[0m\n'
    printf 'plain line\n'
    printf '\033]0;some title\007after the OSC\n'
    ;;
burst)
    for i in $(seq 1 200); do
        echo "burst $i"
    done
    ;;
slow)
    for i in $(seq 1 20); do
        echo "slow $i"
        sleep 0.5
    done
    ;;
binary)
    printf 'leading text\n'
    printf '\000garbage\xff\xfe\x01\x02\n'
    printf 'trailing text\n'
    ;;
*)
    echo "unknown mode: $mode" >&2
    echo "modes: short long ansi burst slow binary" >&2
    exit 2
    ;;
esac
