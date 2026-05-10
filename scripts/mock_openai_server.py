#!/usr/bin/env python3
"""Mock OpenAI-compatible server for end-to-end testing of hax's HTTP/SSE
plumbing — auto-retry, mid-stream error recovery, idle spinner, length
truncation, tool dispatch.

The unit tests cover each piece in isolation (retry classifier, turn state
machine, etc.); this server is the manual integration check for the bits
that only show up at the wire boundary, where libcurl, the SSE parser,
the openai_events translator, and the agent's UX all interact.

Modes
-----
  normal      Stream a short reply with usage chunk and [DONE]. Baseline.
  500/503/429 Always fail with this status. Exercises the retry loop and,
              after exhausting retries, the EV_ERROR surface.
  flaky-NNN   Fail with status NNN for the first --fail-count requests,
              then serve `normal`. Exercises retry *recovery* — the
              spinner countdown should appear, then the response should
              come through cleanly.
  mid-drop    Stream a few text deltas, then close the socket without
              emitting `[DONE]` or a finish_reason. openai_events_finalize
              surfaces this as EV_ERROR("stream ended before completion").
              Verifies the new "preserve partial text + tag with
              [interrupted]" behaviour: the streamed bytes should both
              stay on screen AND land in conversation history, so a
              follow-up "continue" turn carries them.
  truncated   Stream a few deltas, then emit finish_reason=length.
              Same preserve-and-tag behaviour as `mid-drop` but via the
              length/content_filter EV_ERROR path inside openai_events.
  mid-tool    Stream text + a completed tool_call, then close mid-stream
              before any further content. Verifies the synthetic
              [interrupted] tool_result the agent inserts so the
              conversation stays well-formed (a tool_call without a
              result is malformed history that future turns reject).
  mid-args    Stream text + a tool_call's start/args (NO finish_reason
              yet, so EV_TOOL_CALL_END is never emitted) + drop. The
              earlier EV_TOOL_CALL_START already flushed the text into
              the turn's items and cleared in_text, so when the agent's
              error handler runs, in_text=0 and there are no completed
              tool_calls — the helper returns 0. This pins the
              "fallback standalone marker" path in agent.c; without
              it, the model would see a complete-looking assistant
              response with no signal it was cut short.
  sse-error   Return HTTP 503 with an SSE-shaped error body
              (`data: {"error":...}`). Verifies that http.c suppresses
              SSE parser feeding for non-2xx responses — otherwise the
              inline error events would land in the live turn before
              the retry loop classifies the status, and t->error
              (sticky) would taint a successful retry attempt.
  slow        Long pauses between deltas. Exercises the idle-text
              spinner that appears when the model goes quiet mid-text.
  tool-call   Emit a single bash tool_call. Exercises the verbose
              tool-dispatch UI without touching error paths.

Usage
-----
  Terminal 1:
      python3 scripts/mock_openai_server.py --mode flaky-500 --fail-count 2

  Terminal 2:
      HAX_PROVIDER=openai-compatible \
      HAX_OPENAI_BASE_URL=http://127.0.0.1:47821/v1 \
      HAX_OPENAI_API_KEY=test \
      HAX_MODEL=mock \
      ./build/hax

  Then type any prompt. The mock ignores prompt content — what you see is
  driven entirely by --mode.

  For faster retry feedback when stress-testing the backoff loop, lower
  the base delay (parse_duration_ms grammar — bare numbers are seconds):
      HAX_HTTP_RETRY_BASE=100ms ./build/hax

  --mode is sticky for the server's lifetime; restart the server to switch.
  --fail-count only affects flaky-* modes.
"""

import argparse
import json
import socket
import socketserver
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL_ID = "mock"


def _delta_chunk(text):
    return json.dumps({
        "id": "chatcmpl-mock",
        "object": "chat.completion.chunk",
        "model": MODEL_ID,
        "choices": [{"index": 0, "delta": {"content": text}, "finish_reason": None}],
    })


def _finish_chunk(reason):
    return json.dumps({
        "id": "chatcmpl-mock",
        "object": "chat.completion.chunk",
        "model": MODEL_ID,
        "choices": [{"index": 0, "delta": {}, "finish_reason": reason}],
    })


def _usage_chunk(in_tok, out_tok):
    return json.dumps({
        "id": "chatcmpl-mock",
        "object": "chat.completion.chunk",
        "model": MODEL_ID,
        # Matching real backends: the trailing usage chunk has empty choices
        # and the usage object alongside.
        "choices": [],
        "usage": {
            "prompt_tokens": in_tok,
            "completion_tokens": out_tok,
            "total_tokens": in_tok + out_tok,
        },
    })


def _tool_call_chunk(call_id, name, args):
    return json.dumps({
        "id": "chatcmpl-mock",
        "object": "chat.completion.chunk",
        "model": MODEL_ID,
        "choices": [{
            "index": 0,
            "delta": {
                "tool_calls": [{
                    "index": 0,
                    "id": call_id,
                    "type": "function",
                    "function": {"name": name, "arguments": args},
                }],
            },
            "finish_reason": None,
        }],
    })


class MockHandler(BaseHTTPRequestHandler):
    # Class-level state shared across handler instances. Python ints are
    # atomic for read/modify/write under the GIL; if we ever need finer
    # accounting (e.g. per-route counters), promote to a Lock.
    mode = "normal"
    fail_count = 2
    request_n = 0
    counter_lock = threading.Lock()

    # `format` shadows the builtin here, but BaseHTTPRequestHandler's
    # signature uses that exact name and strict type checkers (Pyright,
    # Pylance) treat the parameter name as part of the override contract.
    def log_message(self, format, *args):  # noqa: A002 — override compat
        sys.stderr.write("[mock] " + (format % args) + "\n")

    # --- routing -----------------------------------------------------------

    def do_GET(self):
        # /v1/models is harmless to support even though openai-compatible
        # doesn't probe it; some users will point llama.cpp preset here too,
        # which DOES probe.
        if self.path.startswith("/v1/models"):
            body = json.dumps({
                "object": "list",
                "data": [{"id": MODEL_ID, "object": "model"}],
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return

        # Drain the request body so the connection is in a clean state for
        # whatever response we're about to write. Content is ignored —
        # behaviour is driven by --mode.
        clen = int(self.headers.get("Content-Length", 0))
        if clen:
            self.rfile.read(clen)

        with self.counter_lock:
            type(self).request_n += 1
            n = type(self).request_n

        mode = self.mode

        # --- pre-stream failure modes --------------------------------------
        if mode in ("500", "503", "429"):
            self.send_error(int(mode))
            return
        if mode.startswith("flaky-"):
            status = int(mode.split("-", 1)[1])
            if n <= self.fail_count:
                self.send_error(status)
                return
            # else fall through to the normal path

        # --- non-2xx with an SSE-shaped body --------------------------------
        # Special case: status 503 + Content-Type: text/event-stream + an
        # inline error event. Tests the http.c gate that suppresses SSE
        # parser feeding on non-success responses.
        if mode == "sse-error":
            self.send_response(503)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()
            self._write('{"error":{"message":"upstream rate limit","type":"rate_limit_error"}}')
            return

        # --- streaming responses -------------------------------------------
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        # Connection: close keeps the lifecycle simple — no keepalive
        # state to reason about across modes.
        self.send_header("Connection", "close")
        self.end_headers()

        if mode == "mid-drop":
            self._serve_mid_drop()
        elif mode == "mid-tool":
            self._serve_mid_tool()
        elif mode == "mid-args":
            self._serve_mid_args()
        elif mode == "truncated":
            self._serve_truncated()
        elif mode == "slow":
            self._serve_slow()
        elif mode == "tool-call":
            self._serve_tool_call(n)
        else:
            # normal (also fallthrough from a recovered flaky-* request)
            self._serve_normal()

    # --- mode handlers -----------------------------------------------------

    def _serve_normal(self):
        for word in ["Hello ", "from ", "the ", "mock ", "server.", ""]:
            if word:
                self._write(_delta_chunk(word))
                time.sleep(0.05)
        self._write(_finish_chunk("stop"))
        self._write(_usage_chunk(12, 6))
        self._write_done()

    def _serve_slow(self):
        # 2s between tokens — enough to trigger the idle inline glyph
        # (TEXT_IDLE_TIMEOUT_MS = 1500ms) but not so long that a casual
        # tester gives up. Adjust by editing the sleep.
        for word in ["This ", "is ", "a ", "slow ", "response."]:
            self._write(_delta_chunk(word))
            time.sleep(2.0)
        self._write(_finish_chunk("stop"))
        self._write(_usage_chunk(8, 5))
        self._write_done()

    def _serve_mid_drop(self):
        # A few deltas, then we just close the socket without sending
        # [DONE] or a finish_reason. The libcurl side sees a clean EOF
        # mid-stream; openai_events_finalize then converts the missing
        # terminal marker into EV_ERROR("stream ended before completion").
        self._write(_delta_chunk("Let me think about this. The answer is "))
        time.sleep(0.05)
        self._write(_delta_chunk("probably going to be"))
        time.sleep(0.05)
        # Just stop. The TCPServer / Connection: close header takes care
        # of actually shutting the socket once do_POST returns.

    def _serve_mid_tool(self):
        # Text → completed tool_call (finish_reason=tool_calls so the
        # openai_events translator emits EV_TOOL_CALL_END and the call
        # actually lands in turn->items) → abrupt TCP RST before [DONE].
        # We can't use a clean close here: with saw_finish=1, finalize
        # would emit EV_DONE (treating the close as a successful
        # response), the agent would dispatch the tool, and we'd loop.
        # An RST forces libcurl to return rc != 0, which the openai.c
        # provider surfaces as EV_ERROR — exactly the path that
        # triggers the agent's [interrupted] tool_result synthesis
        # (ensures a tool_call without a matching result doesn't land
        # in conversation history as malformed state).
        self._write(_delta_chunk("Let me run that for you. "))
        time.sleep(0.05)
        self._write(_tool_call_chunk("call_mid_1", "bash", '{"command":"echo hi"}'))
        self._write(_finish_chunk("tool_calls"))
        # SO_LINGER (l_onoff=1, l_linger=0): on close(), the kernel
        # sends RST instead of FIN. libcurl treats this as a transport
        # error (CURLE_RECV_ERROR / CURLE_PARTIAL_FILE depending on
        # state) and our retry classifier maps status=200+rc!=0 to
        # "don't retry" (mid-stream drop), so the EV_ERROR fires once
        # and the agent's preserve+synth path takes over.
        linger = struct.pack("ii", 1, 0)
        self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        self.connection.close()

    def _serve_mid_args(self):
        # Text → tool_call_start + partial args (NO finish_reason) →
        # clean drop. EV_TOOL_CALL_START flushed the text into the
        # turn's items and cleared in_text; the tool_call stays in
        # pending (never finalized). Without finish_reason,
        # openai_events_finalize emits EV_ERROR("stream ended before
        # completion"). The agent should notice in_text=0 +
        # no-completed-tool_calls and fall back to a standalone
        # [interrupted] marker so the model doesn't see a
        # complete-looking response.
        self._write(_delta_chunk("Let me check the file. "))
        time.sleep(0.05)
        self._write(_tool_call_chunk("call_args_1", "read", '{"path":"x.c'))
        # Drop here. No more args, no end, no finish_reason, no [DONE].

    def _serve_truncated(self):
        # Same preserve-and-tag path, but reached via finish_reason=length
        # inside openai_events instead of a transport drop. Useful to
        # confirm both paths converge on the same EV_ERROR handling.
        self._write(_delta_chunk("This response is being cut off because the "))
        time.sleep(0.05)
        self._write(_delta_chunk("token budget ran out mid-thought and"))
        self._write(_finish_chunk("length"))
        self._write_done()

    def _serve_tool_call(self, n):
        # On the FIRST request we emit a tool_call and let the agent
        # dispatch the tool. On the follow-up (the agent always loops
        # back with the tool result) we close the cycle with a plain
        # text response — otherwise the agent would re-receive a
        # tool_call every turn and loop forever. `n` is the 1-based
        # request counter from do_POST.
        if n == 1:
            self._write(_delta_chunk("Running a quick command. "))
            self._write(_tool_call_chunk("call_1", "bash", '{"command":"echo hello"}'))
            self._write(_finish_chunk("tool_calls"))
            self._write(_usage_chunk(15, 8))
        else:
            self._write(_delta_chunk("The command output was 'hello'."))
            self._write(_finish_chunk("stop"))
            self._write(_usage_chunk(20, 8))
        self._write_done()

    # --- low-level write helpers ------------------------------------------

    def _write(self, payload):
        try:
            self.wfile.write(b"data: " + payload.encode() + b"\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            # Client cancelled (Esc in hax) — stop writing; do_POST will
            # return and the socket will close cleanly.
            raise

    def _write_done(self):
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--port", type=int, default=47821)
    ap.add_argument("--mode", default="normal", choices=[
        "normal", "500", "503", "429",
        "flaky-500", "flaky-503", "flaky-429",
        "mid-drop", "mid-tool", "mid-args", "sse-error",
        "truncated", "slow", "tool-call",
    ])
    ap.add_argument("--fail-count", type=int, default=2,
                    help="for flaky-* modes: how many requests fail before success")
    args = ap.parse_args()

    MockHandler.mode = args.mode
    MockHandler.fail_count = args.fail_count

    addr = ("127.0.0.1", args.port)
    # ThreadingHTTPServer + SO_REUSEADDR so Ctrl-C / restart cycle is
    # painless during interactive testing. Each request runs in its own
    # thread, which matters for `slow` mode where one in-flight request
    # would otherwise block a parallel cancel.
    socketserver.TCPServer.allow_reuse_address = True
    with ThreadingHTTPServer(addr, MockHandler) as srv:
        sys.stderr.write(
            f"[mock] listening on http://{addr[0]}:{addr[1]}/v1 "
            f"(mode={args.mode}"
            f"{f', fail-count={args.fail_count}' if args.mode.startswith('flaky-') else ''})\n"
        )
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            sys.stderr.write("\n[mock] shutting down\n")


if __name__ == "__main__":
    main()
