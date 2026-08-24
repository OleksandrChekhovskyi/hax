"""Python binding for hax, built on libhax through cffi.

hax keeps its configuration, provider selection, and diagnostics in process-wide state, so this
module drives exactly one agent per process. Agent enforces that rather than letting a second
instance silently share the first one's settings.

Meson builds the _hax_cffi extension; nothing here compiles anything:

    meson setup build-embed -Dembed=true && meson compile -C build-embed

HAX_EXTENSION_DIR points at the directory holding it. Without that, the build directories beside
the source tree are searched, so a plain `meson compile` is enough to make `import hax` work.
"""

from __future__ import annotations

import json
import sys
import threading
from typing import Any, Callable

def _locate_extension() -> None:
    """Put the meson-built extension on sys.path, preferring an explicit choice."""
    import os
    from pathlib import Path

    explicit = os.environ.get("HAX_EXTENSION_DIR")
    candidates = [Path(explicit)] if explicit else []
    repo_root = Path(__file__).resolve().parents[3]
    candidates += [repo_root / name / "bindings" for name in ("build-embed", "build")]
    for candidate in candidates:
        if candidate.is_dir() and any(candidate.glob("_hax_cffi*")):
            sys.path.insert(0, str(candidate))
            return


_locate_extension()

try:
    from _hax_cffi import ffi, lib
except ImportError as exc:  # pragma: no cover - build guidance, not a runtime path
    raise ImportError(
        "_hax_cffi is not built; run: meson setup build-embed -Dembed=true "
        "&& meson compile -C build-embed"
    ) from exc

__all__ = ["Agent", "HaxError", "HaxProviderError"]


class HaxError(Exception):
    """A hax diagnostic or a binding-level failure."""


class HaxProviderError(HaxError):
    """The provider stream failed or was rejected."""


def _check_abi() -> None:
    """Catch a libhax swapped underneath an extension built against different headers.

    cffi already agreed with the headers at compile time; this covers the runtime case those
    checks cannot see.
    """
    abi = lib.hax_abi()
    if abi.version != lib.HAX_ABI_VERSION:
        raise HaxError(
            f"libhax reports ABI version {abi.version}, this extension was built against "
            f"{lib.HAX_ABI_VERSION}"
        )
    sizes = {
        "struct item": (abi.sizeof_item, ffi.sizeof("struct item")),
        "struct agent_session": (abi.sizeof_agent_session, ffi.sizeof("struct agent_session")),
        "struct agent_loop_params": (
            abi.sizeof_agent_loop_params,
            ffi.sizeof("struct agent_loop_params"),
        ),
        "struct agent_loop_result": (
            abi.sizeof_agent_loop_result,
            ffi.sizeof("struct agent_loop_result"),
        ),
        "struct agent_loop_hooks": (
            abi.sizeof_agent_loop_hooks,
            ffi.sizeof("struct agent_loop_hooks"),
        ),
    }
    bad = [f"{name}: library {theirs}, extension {mine}" for name, (theirs, mine) in sizes.items() if theirs != mine]
    if bad:
        raise HaxError("libhax struct sizes differ from this extension:\n  " + "\n  ".join(bad))


_check_abi()

_KINDS = {
    lib.ITEM_USER_MESSAGE: "user",
    lib.ITEM_ASSISTANT_MESSAGE: "assistant",
    lib.ITEM_TOOL_CALL: "tool_call",
    lib.ITEM_TOOL_RESULT: "tool_result",
    lib.ITEM_REASONING: "reasoning",
    lib.ITEM_TURN_BOUNDARY: "boundary",
    lib.ITEM_TURN_USAGE: "usage",
}

_OUTCOMES = {
    lib.AGENT_LOOP_COMPLETE: "complete",
    lib.AGENT_LOOP_PROVIDER_ERROR: "provider_error",
    lib.AGENT_LOOP_INTERRUPTED: "interrupted",
    lib.AGENT_LOOP_PAUSED: "paused",
    lib.AGENT_LOOP_MAX_TURNS: "max_turns",
}


def _text(value) -> str:
    return ffi.string(value).decode("utf-8", "replace") if value != ffi.NULL else ""


# The trampolines below run on the thread inside agent_loop_run; cffi reacquires the GIL for each.
# `user` carries the handle made by Agent, so the callbacks stay module-level while the state
# stays per-instance.


@ffi.def_extern()
def hax_py_diag(level, message, user) -> None:
    agent = ffi.from_handle(user) if user != ffi.NULL else None
    if agent is not None:
        agent._diagnostics.append(_text(message))


@ffi.def_extern()
def hax_py_checkpoint(user) -> int:
    agent = ffi.from_handle(user)
    return lib.AGENT_LOOP_SIG_ABORT if agent._pending_exc else lib.AGENT_LOOP_SIG_NONE


@ffi.def_extern()
def hax_py_tool_call(call, action, image_input, user):
    """Return an owned result on every path: a call the loop cannot pair corrupts history."""
    agent = ffi.from_handle(user)
    try:
        if action != lib.AGENT_LOOP_TOOL_RUN:
            return lib.agent_tool_result_make(call, b"[skipped]", ffi.NULL)

        fn = agent._tools.get(_text(call.tool_name))
        if fn is None:
            return agent._run_builtin(call, image_input)

        arguments = json.loads(_text(call.tool_arguments_json) or "{}")
        output = fn(**arguments)
        text = "" if output is None else str(output)
        return lib.agent_tool_result_make(call, text.encode(), ffi.NULL)
    except BaseException:
        # A Python exception cannot unwind through agent_loop_run. Stash it, ask the loop to stop,
        # and still hand back a well-formed result.
        agent._pending_exc = sys.exc_info()
        lib.cancel_request_abort()
        return lib.agent_tool_result_make(call, b"error: the host tool raised an exception",
                                          ffi.NULL)


class Agent:
    """One conversation against one provider.

    hax's configuration is process-wide, so only one Agent may exist at a time; constructing a
    second one raises. Use it as a context manager, or call close() when finished.
    """

    _live: "Agent | None" = None
    _guard = threading.Lock()

    def __init__(
        self,
        provider: str | None = None,
        model: str | None = None,
        *,
        system_prompt: str | None = None,
        max_turns: int = 100,
        record_session: bool = False,
    ):
        with Agent._guard:
            if Agent._live is not None:
                raise HaxError(
                    "an Agent already exists; hax keeps process-wide state, so close the first one"
                )
            Agent._live = self

        self._closed = False
        self._provider = ffi.NULL
        self._session = None
        # Snapshot taken at close() so the conversation stays readable after the context manager
        # exits, which is when callers usually want to inspect it.
        self._final_items: list[dict[str, Any]] | None = None
        self._diagnostics: list[str] = []
        self._pending_exc = None
        self._tools: dict[str, Callable[..., Any]] = {}
        self._max_turns = max_turns
        # Keep the handle alive for as long as C may call back through it.
        self._handle = ffi.new_handle(self)

        try:
            options = ffi.new(
                "struct hax_embed_options *",
                {
                    # The host owns its locale: setenv() races any thread reading the environment.
                    "own_locale": 0,
                    "own_curl_global": 1,
                    "own_atexit": 0,
                    "diag": lib.hax_py_diag,
                    "diag_user": self._handle,
                },
            )
            if lib.hax_init(options) != 0:
                raise HaxError(self._last_diagnostic("hax_init failed"))

            # Overrides go in after hax_init(): config_init() builds the store they live in.
            if not record_session:
                lib.config_set_override(b"no_session", b"1")
            if provider:
                lib.config_set_override(b"provider", provider.encode())
            if model:
                lib.config_set_override(b"model", model.encode())
            if system_prompt is not None:
                lib.config_set_override(b"system_prompt", system_prompt.encode())

            self._provider = lib.hax_provider_new(
                provider.encode() if provider else ffi.NULL
            )
            if self._provider == ffi.NULL:
                lib.hax_shutdown()
                raise HaxError(self._last_diagnostic("could not create a provider"))

            self._session = ffi.new("struct agent_session *")
            opts = ffi.new("struct hax_opts *", {"raw": 0, "resume_path": ffi.NULL,
                                                 "provider_autoselected": 0})
            lib.agent_session_init(self._session, self._provider, opts)
        except BaseException:
            with Agent._guard:
                Agent._live = None
            raise

    # --- lifecycle ---

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._session is not None:
            self._final_items = self.items
            lib.agent_session_free(self._session)
            self._session = None
        if self._provider != ffi.NULL:
            lib.hax_provider_destroy(self._provider)
            self._provider = ffi.NULL
        lib.hax_shutdown()
        with Agent._guard:
            if Agent._live is self:
                Agent._live = None

    def __enter__(self) -> "Agent":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    # --- tools ---

    def tool(self, fn: Callable[..., Any]) -> Callable[..., Any]:
        """Register a Python tool. It shadows a built-in of the same name."""
        self._tools[fn.__name__] = fn
        return fn

    def _run_builtin(self, call, image_input: int):
        ctx = ffi.new("struct tool_run_ctx *", {"image_input": image_input})
        tc = ffi.new("struct agent_tool_call *")
        lib.agent_tool_call_init(tc, call)
        try:
            output = lib.agent_tool_call_run(tc, ctx)
            result = lib.agent_tool_result_make(call, output, ctx)
            lib.free(output)
            return result
        finally:
            lib.agent_tool_call_destroy(tc)

    def _last_diagnostic(self, fallback: str) -> str:
        return self._diagnostics[-1] if self._diagnostics else fallback

    # --- running ---

    def send(self, prompt: str) -> str:
        """Run one user turn and return the final assistant text."""
        if self._closed:
            raise HaxError("this Agent is closed")

        lib.cancel_clear_requests()
        self._pending_exc = None
        before = self._session.n_items

        lib.agent_session_add_user(self._session, prompt.encode())

        params = ffi.new(
            "struct agent_loop_params *",
            {
                "session": self._session,
                "provider": self._provider,
                "tlog": ffi.NULL,
                "slog": ffi.NULL,
                "max_turns": self._max_turns,
                "continued": 0,
                "hooks": {
                    "user": self._handle,
                    "checkpoint": lib.hax_py_checkpoint,
                    "tool_call": lib.hax_py_tool_call,
                },
            },
        )
        result = ffi.new("struct agent_loop_result *")
        lib.agent_loop_run(params, result)

        outcome = result.outcome
        error = _text(result.error_message)
        lib.agent_loop_result_destroy(result)

        if self._pending_exc is not None:
            _, exc, tb = self._pending_exc
            self._pending_exc = None
            raise exc.with_traceback(tb)
        if outcome == lib.AGENT_LOOP_PROVIDER_ERROR:
            raise HaxProviderError(error or self._last_diagnostic("provider error"))
        if outcome not in (lib.AGENT_LOOP_COMPLETE, lib.AGENT_LOOP_MAX_TURNS):
            raise HaxError(f"turn ended {_OUTCOMES.get(outcome, outcome)}")

        return "\n".join(
            item["text"]
            for item in self.items[before:]
            if item["kind"] == "assistant" and item["text"]
        )

    # --- inspection ---

    @property
    def items(self) -> list[dict[str, Any]]:
        """The conversation so far, as plain dicts. Readable after close()."""
        if self._final_items is not None:
            return list(self._final_items)
        out = []
        for i in range(self._session.n_items):
            item = self._session.items[i]
            out.append(
                {
                    "kind": _KINDS.get(item.kind, item.kind),
                    "text": _text(item.text),
                    "call_id": _text(item.call_id),
                    "tool_name": _text(item.tool_name),
                    "arguments": _text(item.tool_arguments_json),
                    "output": _text(item.output),
                }
            )
        return out

    @property
    def diagnostics(self) -> list[str]:
        """Every hax diagnostic emitted since construction."""
        return list(self._diagnostics)

    @property
    def model(self) -> str:
        if self._closed:
            raise HaxError("this Agent is closed")
        return _text(self._session.model)
