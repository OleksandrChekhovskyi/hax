"""One-shot --json emits JSONL progress events with the result as the last event."""

import json

import harness

result = harness.run_oneshot("go", "tool_roundtrip.txt", extra_args=["--json"])
harness.expect(result.returncode == 0, "exit status is 0", result)

events = []
for line in result.stdout.splitlines():
    parsed = json.loads(line)  # every stdout line must be valid JSON
    events.append(parsed.get("type"))
harness.expect(events == ["turn_start", "turn_end", "tool_use", "turn_start", "turn_end",
                          "result"], f"event sequence is {events}", result)
harness.expect("hax: " not in result.stdout, "no banner text on stdout", result)
