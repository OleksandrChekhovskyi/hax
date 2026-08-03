#!/usr/bin/env python3
"""A scripted bash tool call executes in the scratch working directory."""

import harness

result = harness.run_oneshot("go", "tool_roundtrip.txt")
harness.expect(result.returncode == 0, "exit status is 0", result)

out_file = result.workdir / "out.txt"
harness.expect(out_file.exists(), "bash tool call created out.txt", result)
harness.expect(out_file.read_text() == "marker42\n", "out.txt has the scripted content", result)
harness.expect("Tool finished." in result.stdout, "final assistant text reaches stdout", result)
