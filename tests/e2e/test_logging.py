#!/usr/bin/env python3
"""Trace and transcript destinations are created and populated in one-shot mode."""

import harness


for variable, filename in (
    ("HAX_TRACE", "trace.log"),
    ("HAX_TRANSCRIPT", "transcript.log"),
):
    result = harness.run_oneshot("hi", "hello.txt", {variable: filename})
    harness.expect(result.returncode == 0, f"{variable} run exits 0", result)
    path = result.workdir / filename
    harness.expect(path.is_file(), f"{variable} creates its destination", result)
    if variable == "HAX_TRANSCRIPT":
        harness.expect("hi" in path.read_text(encoding="utf-8"), "transcript records prompt", result)
