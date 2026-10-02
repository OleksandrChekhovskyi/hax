#!/usr/bin/env python3
"""One-shot mode (-p): what reaches stdout and stderr, the exit status, and resuming an
interrupted run."""

import json
import signal
import time

import harness

# The mock provider defaults to an unrecorded session; a session id and resuming need the file.
RECORD = {"HAX_NO_SESSION": "0"}


def test_text_and_banner():
    """The scripted assistant text reaches stdout and the start banner, on stderr, names the model
    and session."""
    result = harness.run_oneshot(
        "hi",
        """
        text Hello from mock
        end-turn
        """,
        extra_env=RECORD,
    )
    harness.expect(result.returncode == 0, "exit status is 0", result)
    harness.expect("Hello from mock" in result.stdout, "scripted text reaches stdout", result)

    banner = result.stderr.splitlines()[0]
    harness.expect("mock-model" in banner, "start banner names the model on stderr", result)
    # Announced before the run, so a run killed outright is still resumable.
    _, separator, session_id = banner.rpartition(" · session ")
    harness.expect(
        bool(separator and session_id.strip()), "start banner names the session id", result
    )


def test_tool_runs_in_workdir():
    """A scripted bash tool call executes in the scratch working directory."""
    result = harness.run_oneshot(
        "go",
        """
        text Running a command
        tool bash {"command":"echo marker42 > out.txt"}
        end-turn

        text Tool finished.
        end-turn
        """,
    )
    harness.expect(result.returncode == 0, "exit status is 0", result)

    out_file = result.workdir / "out.txt"
    harness.expect(out_file.exists(), "bash tool call created out.txt", result)
    harness.expect(out_file.read_text() == "marker42\n", "out.txt has the scripted content", result)
    harness.expect("Tool finished." in result.stdout, "final assistant text reaches stdout", result)


def test_json_stream():
    """--json turns stdout into a JSONL stream: a session record, conversation records in
    session-file schema, and a terminal result record."""
    result = harness.run_oneshot(
        "go",
        """
        text Running a command
        tool bash {"command":"echo marker42 > out.txt"}
        end-turn

        text Tool finished.
        end-turn
        """,
        extra_args=["--json"],
    )
    harness.expect(result.returncode == 0, "exit status is 0", result)

    lines = result.stdout.splitlines()
    harness.expect(len(lines) >= 3, "stream has session, item, and result records", result)
    records = []
    for line in lines:
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            harness.expect(False, f"stdout line is not JSON: {line!r}", result)

    session = records[0]
    harness.expect(session.get("type") == "session", "stream opens with a session record", result)
    harness.expect(session.get("model") == "mock-model", "session record names the model", result)

    kinds = [record.get("kind") for record in records]
    harness.expect("user" in kinds, "the prompt appears as a user record", result)
    tool_calls = [r for r in records if r.get("kind") == "tool_call"]
    harness.expect(
        any(r.get("tool_name") == "bash" for r in tool_calls),
        "the scripted bash call appears as a tool_call record",
        result,
    )
    harness.expect("tool_result" in kinds, "the tool result is streamed", result)
    harness.expect("turn_usage" in kinds, "per-turn usage records are streamed", result)

    final = records[-1]
    harness.expect(final.get("type") == "result", "stream closes with a result record", result)
    harness.expect(final.get("outcome") == "complete", "result reports completion", result)
    harness.expect(final.get("text") == "Tool finished.", "result carries the final text", result)
    harness.expect(final.get("turns") == 2, "result counts both model round-trips", result)

    out_file = result.workdir / "out.txt"
    harness.expect(out_file.exists(), "the tool call still executed", result)
    harness.expect(result.stderr == "", "no banner or stats duplicate the stream on stderr", result)


def test_sigint_then_resume():
    """SIGINT interrupts a run gracefully: the in-flight stream is cancelled, partial output is kept
    and marked in the recorded session, the --json stream still closes with a result record, and
    the exit status is 130. A promptless --resume then speaks for the user and completes."""
    # The resumed run replays the interrupted turn, so the stall must be short.
    script = """
        text Partial answer before the interrupt.
        delay 2000
        text Never delivered
        end-turn
    """
    home, workdir = harness.make_home()
    proc = harness.spawn_hax(["--json", "go"], script, home, workdir, extra_env=RECORD)

    lines = []
    for line in proc.stdout:
        lines.append(line)
        if json.loads(line).get("kind") == "user":
            break
    # The user record precedes the provider call; the script streams its first
    # sentence immediately and then stalls for seconds, so this lands mid-stream.
    time.sleep(0.5)
    proc.send_signal(signal.SIGINT)
    out, err = proc.communicate(timeout=20)
    result = harness.spawned_result(proc, "".join(lines) + out, err, workdir)

    harness.expect(result.returncode == 130, "a graceful interrupt exits 130", result)
    records = [json.loads(line) for line in result.stdout.splitlines()]
    # Announced before the run, so a killed one is resumable without reaching a result record.
    harness.expect(bool(records[0].get("id")), "the opening session record carries the id", result)
    final = records[-1]
    harness.expect(final.get("type") == "result", "the stream still closes with a result", result)
    harness.expect(
        final.get("outcome") == "interrupted", "the result reports the interrupt", result
    )
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
        ["--json", f"--resume={session_id}"], script, home, workdir, extra_env=RECORD
    )
    out, err = resume.communicate(timeout=30)
    result = harness.spawned_result(resume, out, err, workdir)

    harness.expect(result.returncode == 0, "the promptless resume completes", result)
    records = [json.loads(line) for line in result.stdout.splitlines()]
    harness.expect(
        any(
            record.get("kind") == "user" and record.get("origin") == "continuation"
            for record in records
        ),
        "the marked tail gets a continuation record",
        result,
    )
    harness.expect(records[-1].get("outcome") == "complete", "the resumed result completes", result)


harness.main(globals())
