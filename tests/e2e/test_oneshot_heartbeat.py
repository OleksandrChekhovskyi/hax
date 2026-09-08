#!/usr/bin/env python3
"""A byte-silent provider stream must keep the run-dir heartbeat alive (HTPR-6251).

The agent worker kills a turn when nothing in hax's private run dir changes for the
idle limit. This scenario holds a real HTTP/SSE response at headers-only silence
(mock_openai_server.py --mode silent) long enough to cross the heartbeat throttle
twice, samples the heartbeat while the child is still running, and then checks that
the run cleaned up after itself.
"""

import os
import re
import subprocess
import sys
import time
from pathlib import Path

import harness

REPO_ROOT = Path(harness.__file__).resolve().parents[2]
SILENT_SECONDS = 22.0  # crosses the 10s heartbeat throttle twice inside the window


def fail(message: str, result: harness.Result | None = None) -> None:
    harness.expect(False, message, result)


def main() -> int:
    home = harness.scratch_dir()
    tmpdir = home / "tmp"
    tmpdir.mkdir()
    server = subprocess.Popen(
        [sys.executable, str(REPO_ROOT / "scripts" / "mock_openai_server.py"),
         "--port", "0", "--mode", "silent", "--silent-seconds", str(SILENT_SECONDS)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
        line = ""
        start = time.monotonic()
        while time.monotonic() - start < 10 and server.stderr:
            line = server.stderr.readline()
            match = re.search(r"listening on http://127\.0\.0\.1:(\d+)/v1", line)
            if match:
                break
        else:
            fail("mock server did not report its port")
            return 1
        port = int(match.group(1))

        env = harness.hermetic_env(home)
        env.update({
            "TMPDIR": str(tmpdir),
            "HAX_PROVIDER": "openai-compatible",
            "HAX_OPENAI_BASE_URL": f"http://127.0.0.1:{port}/v1",
            "HAX_OPENAI_API_KEY": "test",
            "HAX_MODEL": "mock",
        })
        binary = Path(os.environ.get(
            "HAX_BIN", str(REPO_ROOT / "build" / "hax"))).resolve()
        workdir = home / "work"
        workdir.mkdir()
        proc = subprocess.Popen(
            [str(binary), "-p", "hi"], cwd=workdir, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

        # Sample the heartbeat while the request is still byte-silent.
        seen_mtimes = set()
        deadline = time.monotonic() + SILENT_SECONDS + 5
        while time.monotonic() < deadline and proc.poll() is None:
            for path in tmpdir.glob("hax-*/heartbeat-*.json"):
                try:
                    seen_mtimes.add(path.stat().st_mtime_ns)
                except OSError:
                    pass
            if len(seen_mtimes) >= 2:
                break
            time.sleep(0.25)

        stdout, stderr = proc.communicate(timeout=30)
        result = harness.Result(
            subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr),
            workdir,
        )
        harness.expect(proc.returncode == 0, "exit status is 0", result)
        harness.expect("silent response arrived" in stdout, "text reaches stdout", result)
        if len(seen_mtimes) < 2:
            fail(f"heartbeat mtime advanced fewer than 2 times while silent "
                 f"(observed {len(seen_mtimes)})", result)
            return 1

        # The run dir and heartbeat must not outlive the process.
        leftovers = list(tmpdir.glob("hax-*"))
        harness.expect(not leftovers, "run dir is cleaned up at exit", result)
        return 0
    finally:
        server.terminate()
        server.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
