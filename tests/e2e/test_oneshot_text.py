#!/usr/bin/env python3
"""Oneshot mode prints the scripted assistant text on stdout and exits 0."""

import harness

# The mock provider defaults to an unrecorded session; a banner session id needs the file.
result = harness.run_oneshot("hi", "hello.txt", extra_env={"HAX_NO_SESSION": "0"})
harness.expect(result.returncode == 0, "exit status is 0", result)
harness.expect("Hello from mock" in result.stdout, "scripted text reaches stdout", result)

banner = result.stderr.splitlines()[0]
harness.expect("mock-model" in banner, "start banner names the model on stderr", result)
# Announced before the run, so a run killed outright is still resumable.
_, separator, session_id = banner.rpartition(" · session ")
harness.expect(bool(separator and session_id.strip()), "start banner names the session id", result)
