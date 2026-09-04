#!/usr/bin/env python3
"""Run a local OpenAI-compatible server for manual HTTP/SSE integration tests.

Example:

    scripts/mock_openai_server.py --mode flaky-500 --fail-count 2

Then run hax against it:

    export HAX_PROVIDER=openai-compatible
    export HAX_OPENAI_BASE_URL=http://127.0.0.1:47821/v1
    export HAX_OPENAI_API_KEY=test HAX_MODEL=mock
    ./build/hax

The mode controls the response; request content is ignored. Use ``--help`` for
available failure, interruption, slow-stream, and tool-call modes.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL_ID = "mock"
COMPLETION_ID = "chatcmpl-mock"
CHAT_COMPLETIONS_PATH = "/v1/chat/completions"
FAST_CHUNK_DELAY_SECONDS = 0.05
SLOW_CHUNK_DELAY_SECONDS = 2.0
MODE_NAMES = (
    "normal",
    "500",
    "503",
    "429",
    "flaky-500",
    "flaky-503",
    "flaky-429",
    "mid-drop",
    "mid-tool",
    "mid-args",
    "sse-error",
    "truncated",
    "slow",
    "hang",
    "tool-call",
)
MODE_HELP = """\
Modes:
  normal       Complete text and usage stream
  500/503/429  Persistent HTTP failure
  flaky-NNN    HTTP failure followed by a normal stream
  mid-drop     Clean EOF before a finish reason
  mid-tool     Reset after a completed tool call but before [DONE]
  mid-args     Clean EOF during tool-call arguments
  sse-error    HTTP 503 with an SSE-shaped error body
  truncated    finish_reason=length
  slow         Two-second pauses between text chunks
  hang         200 headers, then silence for --fail-delay seconds, then EOF
  tool-call    Bash call followed by a final text response

Timing knobs for retry-indicator checks:
  --fail-delay SECONDS   hold each failing request open this long before answering, the
                         way an overloaded upstream does (default: 0)
  --retry-after SECONDS  add a Retry-After header to failure responses
"""


def delta_chunk(text: str) -> str:
    return json.dumps(
        {
            "id": COMPLETION_ID,
            "object": "chat.completion.chunk",
            "model": MODEL_ID,
            "choices": [
                {
                    "index": 0,
                    "delta": {"content": text},
                    "finish_reason": None,
                }
            ],
        }
    )


def finish_chunk(reason: str) -> str:
    return json.dumps(
        {
            "id": COMPLETION_ID,
            "object": "chat.completion.chunk",
            "model": MODEL_ID,
            "choices": [{"index": 0, "delta": {}, "finish_reason": reason}],
        }
    )


def usage_chunk(input_tokens: int, output_tokens: int) -> str:
    return json.dumps(
        {
            "id": COMPLETION_ID,
            "object": "chat.completion.chunk",
            "model": MODEL_ID,
            "choices": [],
            "usage": {
                "prompt_tokens": input_tokens,
                "completion_tokens": output_tokens,
                "total_tokens": input_tokens + output_tokens,
            },
        }
    )


def tool_call_chunk(call_id: str, name: str, arguments: str) -> str:
    return json.dumps(
        {
            "id": COMPLETION_ID,
            "object": "chat.completion.chunk",
            "model": MODEL_ID,
            "choices": [
                {
                    "index": 0,
                    "delta": {
                        "tool_calls": [
                            {
                                "index": 0,
                                "id": call_id,
                                "type": "function",
                                "function": {
                                    "name": name,
                                    "arguments": arguments,
                                },
                            }
                        ]
                    },
                    "finish_reason": None,
                }
            ],
        }
    )


class MockServer(ThreadingHTTPServer):
    allow_reuse_address = True


class MockHandler(BaseHTTPRequestHandler):
    mode = "normal"
    fail_count = 2
    fail_delay = 0.0
    retry_after = 0
    request_count = 0
    request_count_lock = threading.Lock()

    def log_message(self, format: str, *args: object) -> None:
        sys.stderr.write("[mock] " + (format % args) + "\n")

    def do_GET(self) -> None:
        if not self.path.startswith("/v1/models"):
            self.send_error(404)
            return

        self.send_json(
            {
                "object": "list",
                "data": [{"id": MODEL_ID, "object": "model"}],
            }
        )

    def do_POST(self) -> None:
        if self.path != CHAT_COMPLETIONS_PATH:
            self.send_error(404)
            return

        self.discard_request_body()
        request_number = self.next_request_number()
        try:
            if self.send_pre_stream_failure(request_number):
                return
            self.start_event_stream()
            self.serve_mode(request_number)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Client cancellation is a normal end to a manual test.

    def discard_request_body(self) -> None:
        content_length = int(self.headers.get("Content-Length", 0))
        if content_length:
            self.rfile.read(content_length)

    @classmethod
    def next_request_number(cls) -> int:
        with cls.request_count_lock:
            cls.request_count += 1
            return cls.request_count

    def send_pre_stream_failure(self, request_number: int) -> bool:
        if self.mode in ("500", "503", "429"):
            self.send_failure_status(int(self.mode))
            return True

        if self.mode.startswith("flaky-") and request_number <= self.fail_count:
            self.send_failure_status(int(self.mode.removeprefix("flaky-")))
            return True

        if self.mode == "sse-error":
            self.hold_before_failure()
            self.send_response(503)
            self.send_event_stream_headers()
            self.send_retry_after_header()
            self.end_headers()
            self.write_event(
                '{"error":{"message":"upstream rate limit",'
                '"type":"rate_limit_error"}}'
            )
            return True

        return False

    def hold_before_failure(self) -> None:
        if self.fail_delay > 0:
            self.log_message("holding request for %.1fs before failing", self.fail_delay)
            time.sleep(self.fail_delay)

    def send_retry_after_header(self) -> None:
        if self.retry_after > 0:
            self.send_header("Retry-After", str(self.retry_after))

    def send_failure_status(self, status: int) -> None:
        self.hold_before_failure()
        body = json.dumps({"error": {"message": f"mock failure {status}"}}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_retry_after_header()
        self.end_headers()
        self.wfile.write(body)

    def start_event_stream(self) -> None:
        self.send_response(200)
        self.send_event_stream_headers()
        self.end_headers()

    def send_event_stream_headers(self) -> None:
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")

    def send_json(self, value: object) -> None:
        body = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def serve_mode(self, request_number: int) -> None:
        if self.mode == "mid-drop":
            self.serve_mid_drop()
        elif self.mode == "mid-tool":
            self.serve_mid_tool()
        elif self.mode == "mid-args":
            self.serve_mid_args()
        elif self.mode == "truncated":
            self.serve_truncated()
        elif self.mode == "slow":
            self.serve_slow()
        elif self.mode == "hang":
            self.serve_hang(request_number)
        elif self.mode == "tool-call":
            self.serve_tool_call(request_number)
        else:
            self.serve_normal()

    def serve_normal(self) -> None:
        for word in ("Hello ", "from ", "the ", "mock ", "server."):
            self.write_event(delta_chunk(word))
            time.sleep(FAST_CHUNK_DELAY_SECONDS)
        self.write_event(finish_chunk("stop"))
        self.write_event(usage_chunk(12, 6))
        self.write_done()

    def serve_slow(self) -> None:
        for word in ("This ", "is ", "a ", "slow ", "response."):
            self.write_event(delta_chunk(word))
            time.sleep(SLOW_CHUNK_DELAY_SECONDS)
        self.write_event(finish_chunk("stop"))
        self.write_event(usage_chunk(8, 5))
        self.write_done()

    def serve_hang(self, request_number: int) -> None:
        # Headers are already out, so the client sees an accepted request that produces no
        # events. Closing without a finish reason counts as a mid-stream death and is retried.
        if request_number <= self.fail_count:
            self.hold_before_failure()
            return
        self.serve_normal()

    def serve_mid_drop(self) -> None:
        self.write_event(delta_chunk("Let me think about this. The answer is "))
        time.sleep(FAST_CHUNK_DELAY_SECONDS)
        self.write_event(delta_chunk("probably going to be"))
        time.sleep(FAST_CHUNK_DELAY_SECONDS)
        # Returning without a finish reason or [DONE] creates a clean premature EOF.

    def serve_mid_tool(self) -> None:
        self.write_event(delta_chunk("Let me run that for you. "))
        time.sleep(FAST_CHUNK_DELAY_SECONDS)
        self.write_event(
            tool_call_chunk("call_mid_1", "bash", '{"command":"echo hi"}')
        )
        self.write_event(finish_chunk("tool_calls"))
        self.close_with_reset()

    def serve_mid_args(self) -> None:
        self.write_event(delta_chunk("Let me check the file. "))
        time.sleep(FAST_CHUNK_DELAY_SECONDS)
        self.write_event(tool_call_chunk("call_args_1", "read", '{"path":"x.c'))
        # The unfinished arguments leave the tool call pending when the stream closes.

    def serve_truncated(self) -> None:
        self.write_event(delta_chunk("This response is being cut off because the "))
        time.sleep(FAST_CHUNK_DELAY_SECONDS)
        self.write_event(delta_chunk("token budget ran out mid-thought and"))
        self.write_event(finish_chunk("length"))
        self.write_done()

    def serve_tool_call(self, request_number: int) -> None:
        if request_number == 1:
            self.write_event(delta_chunk("Running a quick command. "))
            self.write_event(
                tool_call_chunk("call_1", "bash", '{"command":"echo hello"}')
            )
            self.write_event(finish_chunk("tool_calls"))
            self.write_event(usage_chunk(15, 8))
        else:
            self.write_event(delta_chunk("The command output was 'hello'."))
            self.write_event(finish_chunk("stop"))
            self.write_event(usage_chunk(20, 8))
        self.write_done()

    def write_event(self, payload: str) -> None:
        self.wfile.write(b"data: " + payload.encode() + b"\n\n")
        self.wfile.flush()

    def write_done(self) -> None:
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()

    def close_with_reset(self) -> None:
        # A reset makes libcurl report a transport error even after a finish reason.
        linger = struct.pack("ii", 1, 0)
        self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        self.connection.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=MODE_HELP,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--port", type=int, default=47821)
    parser.add_argument("--mode", default="normal", choices=MODE_NAMES)
    parser.add_argument(
        "--fail-count",
        type=int,
        default=2,
        help="requests to reject before a flaky mode succeeds (default: 2)",
    )
    parser.add_argument(
        "--fail-delay",
        type=float,
        default=0.0,
        help="seconds to hold a failing request open before answering (default: 0)",
    )
    parser.add_argument(
        "--retry-after",
        type=int,
        default=0,
        help="Retry-After header value in seconds for failure responses (default: none)",
    )
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.fail_count < 0:
        parser.error("--fail-count must be non-negative")
    if args.fail_delay < 0:
        parser.error("--fail-delay must be non-negative")
    if args.retry_after < 0:
        parser.error("--retry-after must be non-negative")
    return args


def main() -> int:
    args = parse_args()
    MockHandler.mode = args.mode
    MockHandler.fail_count = args.fail_count
    MockHandler.fail_delay = args.fail_delay
    MockHandler.retry_after = args.retry_after
    MockHandler.request_count = 0

    address = ("127.0.0.1", args.port)
    with MockServer(address, MockHandler) as server:
        flaky_suffix = (
            f", fail-count={args.fail_count}"
            if args.mode.startswith("flaky-") or args.mode == "hang"
            else ""
        )
        if args.fail_delay > 0:
            flaky_suffix += f", fail-delay={args.fail_delay:g}s"
        if args.retry_after > 0:
            flaky_suffix += f", retry-after={args.retry_after}s"
        sys.stderr.write(
            f"[mock] listening on http://{address[0]}:{address[1]}/v1 "
            f"(mode={args.mode}{flaky_suffix})\n"
        )
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            sys.stderr.write("\n[mock] shutting down\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
