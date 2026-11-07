#!/usr/bin/env python3
"""Anthropic browser login, Messages streaming, and token rotation over isolated HTTPS."""

import base64
import concurrent.futures
import hashlib
import http.server
import json
import re
import subprocess
import time

import harness
from https_peer import HttpsPeer


class AnthropicEndpoint(HttpsPeer):
    def __init__(self, home):
        self.grants = []
        self.requests = []
        self.refreshes = 0
        self.reject_access = False
        self.refresh_error = None
        endpoint = self

        class Origin(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def reply(self, status, body, content_type="application/json"):
                if isinstance(body, dict):
                    body = json.dumps(body)
                data = body.encode()
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)

            def authorized(self):
                return not endpoint.reject_access or (
                    self.headers.get("Authorization") == "Bearer access-rotated"
                )

            def do_GET(self):
                endpoint.requests.append((self.path, dict(self.headers)))
                if not self.authorized():
                    self.reply(401, {"error": {"message": "expired"}})
                else:
                    self.reply(200, {"data": [{"id": "claude-test"}], "has_more": False})

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
                if self.path == "/v1/oauth/token":
                    endpoint.grants.append(body)
                    if body.get("grant_type") == "refresh_token":
                        endpoint.refreshes += 1
                        if endpoint.refresh_error:
                            status, error = endpoint.refresh_error
                            self.reply(status, {"error": error})
                            return
                        token = "rotated"
                    else:
                        token = "initial"
                    self.reply(200, {"access_token": f"access-{token}",
                                     "refresh_token": f"refresh-{token}", "expires_in": 3600})
                elif self.path == "/v1/messages":
                    endpoint.requests.append((self.path, dict(self.headers)))
                    if not self.authorized():
                        self.reply(401, {"error": {"message": "expired"}})
                        return
                    chunks = [
                        {"type": "message_start", "message": {"id": "msg-test", "usage": {
                            "input_tokens": 1, "output_tokens": 0}}},
                        {"type": "content_block_start", "index": 0,
                         "content_block": {"type": "text", "text": ""}},
                        {"type": "content_block_delta", "index": 0,
                         "delta": {"type": "text_delta", "text": "Anthropic OAuth verified."}},
                        {"type": "content_block_stop", "index": 0},
                        {"type": "message_delta", "delta": {"stop_reason": "end_turn"},
                         "usage": {"output_tokens": 4}},
                        {"type": "message_stop"},
                    ]
                    events = "".join(
                        f"event: {chunk['type']}\ndata: {json.dumps(chunk)}\n\n" for chunk in chunks
                    )
                    self.reply(200, events, "text/event-stream")
                else:
                    self.reply(404, {})

        super().__init__(home, ("console.anthropic.com", "api.anthropic.com"), Origin)
        self.env["ANTHROPIC_API_KEY"] = ""


def auth_path(home):
    return home / ".local/state/hax/auth.json"


def seed_login(home, lifetime=3600):
    path = auth_path(home)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"anthropic": {
        "access_token": "access-initial", "refresh_token": "refresh-initial",
        "login_id": "login-1", "expires_at": int(time.time()) + lifetime, "generation": 0,
    }}))
    path.chmod(0o600)


def run_anthropic(home, workdir, endpoint):
    env = harness.hermetic_env(home)
    env.update(endpoint.env)
    return subprocess.run(
        [str(harness.hax_binary()), "--provider=anthropic", "--model=claude-test", "-p", "hello"],
        cwd=workdir, env=env, stdin=subprocess.DEVNULL, capture_output=True,
        encoding="utf-8", timeout=15,
    )


def test_browser_login_and_logout():
    home, workdir = harness.make_home()
    # Never launch a real browser from this scenario.
    bin_dir = home / "bin"
    bin_dir.mkdir()
    for name in ("xdg-open", "open"):
        opener = bin_dir / name
        opener.write_text("#!/bin/sh\nexit 0\n")
        opener.chmod(0o755)
    with AnthropicEndpoint(home) as endpoint:
        env = dict(endpoint.env)
        env["PATH"] = f"{bin_dir}:" + harness.hermetic_env(home)["PATH"]
        term = harness.Terminal("", home=home, workdir=workdir, extra_env=env, size=(120, 32))
        term.wait_for("❯")
        term.type("/login anthropic")
        term.send("Enter")
        term.wait_for("authorization code:")
        screen = term.screen().replace("\n", "")
        state = re.search(r"&state=([A-Za-z0-9_-]{43})", screen).group(1)
        challenge = re.search(r"code_challenge=([A-Za-z0-9_-]{43})", screen).group(1)
        term.type(f"authorization-secret#{state}")
        term.send("Enter")
        term.wait_for("logged in to Anthropic")
        harness.expect(auth_path(home).stat().st_mode & 0o777 == 0o600, "login is private")
        grant = endpoint.grants[0]
        actual_challenge = base64.urlsafe_b64encode(
            hashlib.sha256(grant["code_verifier"].encode()).digest()
        ).decode().rstrip("=")
        harness.expect(actual_challenge == challenge, "exchange matches browser PKCE challenge")
        harness.expect(grant["state"] == state, "exchange carries matched state")
        term.type("/logout anthropic")
        term.send("Enter")
        term.wait_for("logged out of anthropic")
        harness.expect("anthropic" not in json.loads(auth_path(home).read_text()), "login removed")
        trace = (home / "trace").read_text()
        harness.expect("authorization-secret" not in trace, "authorization code is redacted")
        harness.expect(grant["code_verifier"] not in trace, "PKCE verifier is redacted")
        term.close()


def test_state_mismatch_preserves_existing_login():
    home, workdir = harness.make_home()
    seed_login(home)
    before = auth_path(home).read_text()
    with AnthropicEndpoint(home) as endpoint:
        # A failed flow never reaches the HTTPS peer.
        term = harness.Terminal("", home=home, workdir=workdir, extra_env={
            **endpoint.env, "PATH": "/nonexistent",
        })
        term.wait_for("❯")
        term.type("/login anthropic")
        term.send("Enter")
        term.wait_for("authorization code:")
        term.type("code#wrong-state")
        term.send("Enter")
        term.wait_for("state mismatch")
        harness.expect(not endpoint.grants, "mismatched state is never exchanged")
        harness.expect(auth_path(home).read_text() == before, "old login is untouched")
        term.close()


def test_refresh_streaming_and_401_recovery():
    for lifetime in (-1, 3600):
        home, workdir = harness.make_home()
        seed_login(home, lifetime)
        with AnthropicEndpoint(home) as endpoint:
            endpoint.reject_access = True
            result = run_anthropic(home, workdir, endpoint)
            harness.expect(result.returncode == 0, f"request succeeds: {result.stderr}")
            harness.expect("Anthropic OAuth verified." in result.stdout, "Messages response streams")
            harness.expect(endpoint.refreshes == 1, "exactly one token rotation")
            stored = json.loads(auth_path(home).read_text())["anthropic"]
            harness.expect(stored["refresh_token"] == "refresh-rotated", "rotation persisted")
            for path, headers in endpoint.requests:
                harness.expect("x-api-key" not in {k.lower() for k in headers}, "no API key in OAuth")
                harness.expect(headers.get("anthropic-beta") == "oauth-2025-04-20", "OAuth beta")
            trace = (home / "trace").read_text()
            harness.expect("refresh-initial" not in trace, "refresh token is redacted")
            harness.expect("access-rotated" not in trace, "access token is redacted")


def test_model_picker_refreshes_credentials():
    for lifetime in (-1, 3600):
        home, workdir = harness.make_home()
        seed_login(home, lifetime)
        with AnthropicEndpoint(home) as endpoint:
            endpoint.reject_access = True
            term = harness.Terminal(
                "", args=["--provider=anthropic", "--model=claude-test"],
                home=home, workdir=workdir, extra_env=endpoint.env,
            )
            term.wait_for("❯")
            term.type("/model")
            term.send("Enter")
            # A single listed model is adopted immediately, continuing to the effort picker.
            term.wait_for("select reasoning effort")
            harness.expect(endpoint.refreshes == 1, "model listing refreshes expired/rejected token")
            harness.expect(any(path.startswith("/v1/models") and
                               headers.get("Authorization") == "Bearer access-rotated"
                               for path, headers in endpoint.requests), "listing retries with new token")
            term.send("Escape")
            term.close()


def test_concurrent_rotation():
    home, workdir = harness.make_home()
    seed_login(home, -1)
    with AnthropicEndpoint(home) as endpoint:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: run_anthropic(home, workdir, endpoint), range(2)))
        for result in results:
            harness.expect(result.returncode == 0, f"concurrent request succeeds: {result.stderr}")
        harness.expect(endpoint.refreshes == 1, "processes share one rotation")


def test_refresh_failure_preserves_only_retryable_login():
    for status, error, retained in ((503, "temporarily_unavailable", True),
                                    (400, "invalid_grant", False)):
        home, workdir = harness.make_home()
        seed_login(home, -1)
        with AnthropicEndpoint(home) as endpoint:
            endpoint.refresh_error = status, error
            result = run_anthropic(home, workdir, endpoint)
            harness.expect(result.returncode != 0, "failed refresh fails request")
            stored = json.loads(auth_path(home).read_text())
            harness.expect(("anthropic" in stored) == retained, "only rejected grant is removed")


harness.main(globals())
