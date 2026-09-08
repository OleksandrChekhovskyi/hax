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
import select
import subprocess
import sys
import time
from pathlib import Path

import harness

REPO_ROOT = Path(harness.__file__).resolve().parents[2]
# The heartbeat throttle is 10s; the silent window must cover a baseline plus two
# throttle-spaced touches, and the assertion below requires that full span.
HEARTBEAT_THROTTLE_APPROX_S = 10.0
SILENT_SECONDS = 2.2 * HEARTBEAT_THROTTLE_APPROX_S


def fail(message: str, result: harness.Result | None = None) -> None:
    harness.expect(False, message, result)


def read_listening_port(pipe, deadline_s: float) -> int | None:
    """Read the mock server's startup line without blocking past the deadline."""
    deadline = time.monotonic() + deadline_s
    line = ""
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        ready, _, _ = select.select([pipe], [], [], min(remaining, 0.5))
        if not ready:
            continue
        chunk = pipe.readline()
        if not chunk:
            return None
        line += chunk
        match = re.search(r"listening on http://127\.0\.0\.1:(\d+)/v1", line)
        if match:
            return int(match.group(1))
    return None


def main() -> int:
    home = harness.scratch_dir()
    tmpdir = home / "tmp"
    tmpdir.mkdir()
    server = subprocess.Popen(
        [sys.executable, str(REPO_ROOT / "scripts" / "mock_openai_server.py"),
         "--port", "0", "--mode", "silent", "--silent-seconds", str(SILENT_SECONDS)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    proc = None
    try:
        port = read_listening_port(server.stderr, 10)
        if port is None:
            fail("mock server did not report its listening port")
            return 1

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

        # Sample only while the request is still byte-silent: advances after the response
        # starts flowing must not count toward the assertion. The server starts holding
        # silence on request receipt (~1s in); cut shortly after its SILENT_SECONDS end.
        seen_mtimes = set()
        first_seen_at: dict[int, float] = {}
        silence_deadline = time.monotonic() + 2.0 + SILENT_SECONDS + 1.0
        while time.monotonic() < silence_deadline and proc.poll() is None:
            for path in tmpdir.glob("hax-*/heartbeat-*.json"):
                try:
                    mtime = path.stat().st_mtime_ns
                except OSError:
                    continue
                if mtime not in first_seen_at:
                    first_seen_at[mtime] = time.monotonic()
                    seen_mtimes.add(mtime)
            time.sleep(0.25)

        try:
            stdout, stderr = proc.communicate(timeout=30)
            returncode = proc.returncode
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
            fail("hax did not exit after the silent window")
            return 1
        result = harness.Result(
            subprocess.CompletedProcess(proc.args, returncode, stdout, stderr),
            workdir,
        )
        harness.expect(returncode == 0, "exit status is 0", result)
        harness.expect("silent response arrived" in stdout, "text reaches stdout", result)
        if len(seen_mtimes) < 3:
            fail(f"heartbeat advanced fewer than 2 times past its baseline while silent "
                 f"(observed {len(seen_mtimes)} distinct mtimes)", result)
            return 1
        span = max(first_seen_at[m] for m in seen_mtimes) - \
            min(first_seen_at[m] for m in seen_mtimes)
        if span < 1.8 * HEARTBEAT_THROTTLE_APPROX_S:
            fail(f"heartbeat advances were not throttle-spaced (span {span:.1f}s)", result)
            return 1

        # The run dir and heartbeat must not outlive the process.
        leftovers = list(tmpdir.glob("hax-*"))
        harness.expect(not leftovers, "run dir is cleaned up at exit", result)
        return 0
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)
        server.terminate()
        server.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
