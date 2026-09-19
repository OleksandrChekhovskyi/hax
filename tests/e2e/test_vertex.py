#!/usr/bin/env python3
"""Vertex AI provider: a loopback Anthropic SSE server stands in for the raw-predict endpoint,
and a one-shot run drives the full path (auth bearer, project/location/model URL, body variant)
to completion."""

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import harness

SSE_RESPONSE = "\n".join(
    [
        # data: lines frame the stream; the anthropic wire dispatches on the JSON "type".
        'data: {"type":"message_start","message":{"id":"msg_smoke","type":"message",'
        '"role":"assistant","model":"claude-sonnet-4-5@20250929","content":[],'
        '"stop_reason":null,"stop_sequence":null,"usage":{"input_tokens":3,"output_tokens":0}}}',
        "",
        'data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}',
        "",
        'data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta",'
        '"text":"Hello from vertex"}}',
        "",
        'data: {"type":"content_block_stop","index":0}',
        "",
        'data: {"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},'
        '"usage":{"output_tokens":3}}',
        "",
        'data: {"type":"message_stop"}',
        "",
    ]
)


class VertexHandler(BaseHTTPRequestHandler):
    request_path = ""
    request_auth = ""

    def do_POST(self):  # noqa: N802 (BaseHTTPRequestHandler API)
        type(self).request_path = self.path
        type(self).request_auth = self.headers.get("Authorization", "")
        body = self.rfile.read(
            int(self.headers.get("Content-Length", 0))
        )
        self.server.request_body = body
        payload = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Connection: close\r\n"
            f"Content-Length: {len(SSE_RESPONSE)}\r\n"
            "\r\n"
        )
        self.wfile.write(payload.encode("utf-8"))
        self.wfile.write(SSE_RESPONSE.encode("utf-8"))
        self.wfile.flush()

    def log_message(self, *args):  # silence the default stderr logging
        pass


def main() -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0), VertexHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_address[1]}"
    model = "claude-sonnet-4-5@20250929"

    home, workdir = harness.make_home()
    xdg_config = home / ".config"
    xdg_config.mkdir()
    (xdg_config / "hax").mkdir()
    (xdg_config / "hax" / "config.json").write_text(
        json.dumps(
            {
                "model": model,
                "providers": {
                    "vertex": {"base_url": base_url, "project": "smoke-proj", "location": "us"}
                },
            }
        )
    )

    env = harness.hermetic_env(home)
    env["GOOGLE_OAUTH_ACCESS_TOKEN"] = "smoke-token"
    env["XDG_CONFIG_HOME"] = str(xdg_config)

    import subprocess

    proc = subprocess.run(
        [str(harness.hax_binary()), "-p", "--provider=vertex", "hi"],
        cwd=workdir,
        env=env,
        capture_output=True,
        encoding="utf-8",
        timeout=30,
    )
    result = harness.Result(proc, workdir)
    server.shutdown()
    server.server_close()

    harness.expect(result.returncode == 0, "vertex one-shot exits 0", result)
    harness.expect("Hello from vertex" in result.stdout, "streamed text reaches stdout", result)
    # The full raw-predict URL with the model in the path (the @version model id intact).
    harness.expect(
        VertexHandler.request_path
        == f"/v1/projects/smoke-proj/locations/us/publishers/anthropic/models/{model}:"
        f"streamRawPredict",
        "endpoint URL carries project/location/model",
        result,
    )
    harness.expect(
        VertexHandler.request_auth == "Bearer smoke-token",
        "request authenticates with the Google bearer token",
        result,
    )
    request_body = getattr(server, "request_body", b"")
    request_json = json.loads(request_body or b"{}")
    harness.expect(
        "model" not in request_json, "raw-predict body carries no model member", result
    )
    harness.expect(
        request_json.get("anthropic_version") == "vertex-2023-10-16",
        "anthropic_version lives in the body",
        result,
    )
    harness.expect(
        request_json.get("max_tokens", 0) > 0, "max_tokens is present", result
    )


if __name__ == "__main__":
    main()
