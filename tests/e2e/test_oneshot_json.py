#!/usr/bin/env python3
"""--json turns stdout into a JSONL stream: a session record, conversation
records in session-file schema, and a terminal result record."""

import json

import harness

result = harness.run_oneshot("go", "tool_roundtrip.txt", extra_args=["--json"])
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
