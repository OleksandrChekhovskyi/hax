#!/usr/bin/env python3
"""SIGINT interrupts a one-shot run gracefully: the in-flight stream is
cancelled, partial output is kept and marked in the recorded session, the
--json stream still closes with a result record, and the exit status is 130.
A promptless --resume then speaks for the user and runs to completion."""

import json
import signal
import time

import harness

# The mock provider defaults to an unrecorded session; resuming needs the file.
RECORD = {"HAX_NO_SESSION": "0"}

home, workdir = harness.make_home()
proc = harness.spawn_hax(["--json", "go"], "interrupt_stall.txt", home, workdir, extra_env=RECORD)

lines = []
for line in proc.stdout:
    lines.append(line)
    if json.loads(line).get("kind") == "user":
        break
# The user record precedes the provider call; the fixture streams its first
# sentence immediately and then stalls for seconds, so this lands mid-stream.
time.sleep(0.5)
proc.send_signal(signal.SIGINT)
out, err = proc.communicate(timeout=20)
result = harness.spawned_result(proc, "".join(lines) + out, err, workdir)

harness.expect(result.returncode == 130, "a graceful interrupt exits 130", result)
records = [json.loads(line) for line in result.stdout.splitlines()]
final = records[-1]
harness.expect(final.get("type") == "result", "the stream still closes with a result", result)
harness.expect(final.get("outcome") == "interrupted", "the result reports the interrupt", result)
harness.expect(
    any(
        record.get("kind") == "assistant"
        and record.get("origin") == "interrupted"
        and "Partial answer" in record.get("text", "")
        for record in records
    ),
    "partial text is kept and marked interrupted",
    result,
)
session_id = final.get("session_id")
harness.expect(bool(session_id), "the interrupted run is resumable", result)

resume = harness.spawn_hax(
    ["--json", f"--resume={session_id}"], "interrupt_stall.txt", home, workdir, extra_env=RECORD
)
out, err = resume.communicate(timeout=30)
result = harness.spawned_result(resume, out, err, workdir)

harness.expect(result.returncode == 0, "the promptless resume completes", result)
records = [json.loads(line) for line in result.stdout.splitlines()]
harness.expect(
    any(record.get("kind") == "user" and record.get("origin") == "continuation" for record in records),
    "the marked tail gets a continuation record",
    result,
)
harness.expect(records[-1].get("outcome") == "complete", "the resumed result completes", result)
