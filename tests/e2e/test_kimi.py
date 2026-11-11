#!/usr/bin/env python3
"""Kimi device login and rotating credentials against an isolated, verified HTTPS peer."""

import concurrent.futures
import http.server
import json
import shutil
import ssl
import subprocess
import threading
import time
import urllib.parse
from pathlib import Path

import harness


class KimiEndpoint:
    def __init__(self, home: Path):
        openssl = shutil.which("openssl")
        if openssl is None:
            harness.skip("openssl is required for the test HTTPS certificate")
        cert, key = home / "cert.pem", home / "key.pem"
        subprocess.run(
            [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
             "-subj", "/CN=auth.kimi.com",
             "-addext", "subjectAltName=DNS:auth.kimi.com,DNS:api.kimi.com",
             "-keyout", str(key), "-out", str(cert)],
            check=True, capture_output=True,
        )
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.pending = False
        self.polls = 0
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

            def do_GET(self):
                if self.path == "/coding/v1/models":
                    self.reply(200, {"data": [{"id": "kimi-for-coding", "context_length": 262144}]})
                else:
                    self.reply(404, {})

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
                if self.path == "/api/oauth/device_authorization":
                    self.reply(200, {
                        "device_code": "device-secret", "user_code": "ABCD-EFGH",
                        "verification_uri_complete": "https://auth.kimi.com/device?code=ABCD-EFGH",
                        "expires_in": 30, "interval": 1,
                    })
                elif self.path == "/api/oauth/token":
                    grant = urllib.parse.parse_qs(body).get("grant_type", [""])[0]
                    if grant == "refresh_token":
                        endpoint.refreshes += 1
                        if endpoint.refresh_error:
                            status, error = endpoint.refresh_error
                            self.reply(status, {"error": error})
                            return
                        token = "rotated"
                    else:
                        endpoint.polls += 1
                        if endpoint.pending or endpoint.polls == 1:
                            self.reply(400, {"error": "authorization_pending"})
                            return
                        token = "initial"
                    self.reply(200, {"access_token": f"access-{token}",
                                     "refresh_token": f"refresh-{token}",
                                     "token_type": "Bearer", "expires_in": 3600})
                elif self.path == "/coding/v1/chat/completions":
                    rejected = self.headers.get("Authorization") != "Bearer access-rotated"
                    if endpoint.reject_access and rejected:
                        self.reply(401, {"error": {"message": "expired"}})
                        return
                    chunks = [
                        {"choices": [{"index": 0, "delta": {"content": "Kimi OAuth verified."}}]},
                        {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
                    ]
                    events = "".join(f"data: {json.dumps(chunk)}\n\n" for chunk in chunks)
                    self.reply(200, events + "data: [DONE]\n\n", "text/event-stream")
                else:
                    self.reply(404, {})

        class Proxy(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_CONNECT(self):
                # No request from this scenario can escape to a real upstream.
                if self.path not in ("auth.kimi.com:443", "api.kimi.com:443"):
                    self.send_error(502)
                    return
                self.send_response(200)
                self.end_headers()
                self.wfile.flush()
                with context.wrap_socket(self.connection, server_side=True) as connection:
                    Origin(connection, self.client_address, self.server)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
        self.thread = threading.Thread(target=self.server.serve_forever)
        proxy = f"http://127.0.0.1:{self.server.server_port}"
        self.env = {
            "https_proxy": proxy, "HTTPS_PROXY": proxy, "ALL_PROXY": "",
            "no_proxy": "", "NO_PROXY": "", "CURL_CA_BUNDLE": str(cert),
            "HAX_CATALOG_URL": "", "HAX_TRACE": str(home / "trace"),
        }

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def auth_path(home):
    return home / ".local/state/hax/auth.json"


def seed_login(home, lifetime=3600):
    path = auth_path(home)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"kimi": {
        "access_token": "access-initial", "refresh_token": "refresh-initial",
        "device_id": "device-1", "login_id": "login-1",
        "expires_at": int(time.time()) + lifetime, "generation": 0,
    }}))
    path.chmod(0o600)


def run_kimi(home, workdir, endpoint):
    env = harness.hermetic_env(home)
    env.update(endpoint.env)
    return subprocess.run(
        [str(harness.hax_binary()), "--provider=kimi", "--model=kimi-for-coding", "-p", "hello"],
        cwd=workdir, env=env, stdin=subprocess.DEVNULL, capture_output=True,
        encoding="utf-8", timeout=15,
    )


def test_device_login_and_local_logout():
    home, workdir = harness.make_home()
    with KimiEndpoint(home) as endpoint:
        term = harness.Terminal(
            "", home=home, workdir=workdir, extra_env=endpoint.env, size=(110, 32),
        )
        term.wait_for("❯")
        term.type("/login")
        term.send("Enter")
        term.wait_for("Kimi Code")
        term.send("Down", "Enter")
        term.wait_for("ABCD-EFGH")
        term.wait_for("logged in to Kimi", timeout=15)
        harness.expect(auth_path(home).stat().st_mode & 0o777 == 0o600, "login file is private")
        term.type("/logout kimi")
        term.send("Enter")
        term.wait_for("logged out of Kimi Code")
        harness.expect("kimi" not in json.loads(auth_path(home).read_text()), "local login removed")
        trace = (home / "trace").read_text()
        harness.expect("device-secret" not in trace, "device code stays out of trace")
        term.close()


def test_device_login_cancellation():
    home, workdir = harness.make_home()
    with KimiEndpoint(home) as endpoint:
        endpoint.pending = True
        term = harness.Terminal("", home=home, workdir=workdir, extra_env=endpoint.env)
        term.wait_for("❯")
        term.type("/login kimi")
        term.send("Enter")
        term.wait_for("waiting for Kimi browser approval")
        term.send("Escape", "Escape")
        term.wait_for("interrupted")
        harness.expect(not auth_path(home).exists(), "cancelled login stores no credentials")
        term.close()


def test_proactive_refresh_and_401_recovery():
    for lifetime in (-1, 3600):
        home, workdir = harness.make_home()
        seed_login(home, lifetime)
        with KimiEndpoint(home) as endpoint:
            endpoint.reject_access = True
            result = run_kimi(home, workdir, endpoint)
            harness.expect(result.returncode == 0, f"Kimi request succeeds: {result.stderr}")
            harness.expect("Kimi OAuth verified." in result.stdout, "response streamed")
            harness.expect(endpoint.refreshes == 1, "exactly one token rotation")
            stored = json.loads(auth_path(home).read_text())["kimi"]
            harness.expect(stored["refresh_token"] == "refresh-rotated", "rotated token persisted")
            trace = (home / "trace").read_text()
            harness.expect("refresh-initial" not in trace, "old refresh token redacted")
            harness.expect("access-rotated" not in trace, "new access token redacted")


def test_concurrent_processes_rotate_only_once():
    home, workdir = harness.make_home()
    seed_login(home, -1)
    with KimiEndpoint(home) as endpoint:
        endpoint.reject_access = True
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(run_kimi, home, workdir, endpoint) for _ in range(2)]
            results = [future.result() for future in futures]
        for result in results:
            harness.expect(result.returncode == 0, f"concurrent request succeeds: {result.stderr}")
        harness.expect(endpoint.refreshes == 1, "processes share one token rotation")


def test_refresh_failure_preserves_only_retryable_login():
    for status, error, retained in ((503, "temporarily_unavailable", True),
                                    (400, "invalid_grant", False)):
        home, workdir = harness.make_home()
        seed_login(home, -1)
        with KimiEndpoint(home) as endpoint:
            endpoint.refresh_error = status, error
            result = run_kimi(home, workdir, endpoint)
            harness.expect(result.returncode != 0, "failed refresh fails request")
            stored = json.loads(auth_path(home).read_text())
            harness.expect(("kimi" in stored) == retained, "only rejected grant is removed")


harness.main(globals())
