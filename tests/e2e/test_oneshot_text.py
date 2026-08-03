#!/usr/bin/env python3
"""Oneshot mode prints the scripted assistant text on stdout and exits 0."""

import harness

result = harness.run_oneshot("hi", "hello.txt")
harness.expect(result.returncode == 0, "exit status is 0", result)
harness.expect("Hello from mock" in result.stdout, "scripted text reaches stdout", result)
harness.expect("mock-model" in result.stderr, "start banner names the model on stderr", result)
