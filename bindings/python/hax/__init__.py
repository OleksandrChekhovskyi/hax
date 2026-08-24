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

import inspect
import json
import sys
import threading
from typing import Any, Callable


def _locate_extension():
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
            return candidate
    return None


_EXTENSION_DIR = _locate_extension()

_BUILD_HINT = (
    "_hax_cffi is not built; run: meson setup build-embed -Dembed=true "
    "&& meson compile -C build-embed"
)


def _import_failure(exc: ImportError) -> ImportError:
    """Explain a found-but-unloadable extension instead of advising a pointless rebuild.

    An extension built by meson's interpreter cannot be imported by a different one, and the
    project makes that easy to hit: the build prefers .venv/bin/python while a bare `python3`
    may be another version entirely. Telling that caller to rebuild sends them in a circle.
    """
    if _EXTENSION_DIR is None:
        return ImportError(_BUILD_HINT)

    import sysconfig
    from pathlib import Path

    # The directory also holds the generated C and meson's private build tree; naming those
    # would only confuse the reader about which file failed to load.
    found = sorted(
        path.name
        for path in Path(_EXTENSION_DIR).glob("_hax_cffi*")
        if path.suffix in (".so", ".dylib", ".pyd")
    )
    suffix = sysconfig.get_config_var("EXT_SUFFIX") or ""
    if any(name.endswith(suffix) for name in found):
        return ImportError(f"{_EXTENSION_DIR} holds {found}, but importing it failed: {exc}")
    return ImportError(
        f"{_EXTENSION_DIR} holds {found}, which {sys.executable} cannot load: it needs a "
        f"*{suffix} extension. That one is built for a different Python, so rebuilding will "
        "not help until meson and this interpreter agree. Run under the interpreter meson "
        "used, or point HAX_EXTENSION_DIR at a build made with this one."
    )


try:
    from _hax_cffi import ffi, lib
except ImportError as exc:  # pragma: no cover - build guidance, not a runtime path
    raise _import_failure(exc) from exc

__all__ = ["Agent", "HaxError", "HaxProviderError", "HaxCancelled"]


class HaxError(Exception):
    """A hax diagnostic or a binding-level failure."""


class HaxProviderError(HaxError):
    """The provider stream failed or was rejected."""


class HaxCancelled(HaxError):
    """The turn stopped early because an abort or pause was requested."""


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

# Agent-authored provenance, named as the session format names it. An ordinary item reports "",
# so a caller can tell a tool result hax wrote from one a tool actually returned.
_ORIGINS = {
    lib.ITEM_ORIGIN_NONE: "",
    lib.ITEM_ORIGIN_COMPACT_SEED: "compact_seed",
    lib.ITEM_ORIGIN_CONTINUATION: "continuation",
    lib.ITEM_ORIGIN_INTERRUPTED: "interrupted",
    lib.ITEM_ORIGIN_SKIPPED: "skipped",
    lib.ITEM_ORIGIN_REFUSED: "refused",
    lib.ITEM_ORIGIN_SUMMARIZED: "summarized",
    lib.ITEM_ORIGIN_TASK_NOTE: "task_note",
}

_OUTCOMES = {
    lib.AGENT_LOOP_COMPLETE: "complete",
    lib.AGENT_LOOP_PROVIDER_ERROR: "provider_error",
    lib.AGENT_LOOP_INTERRUPTED: "interrupted",
    lib.AGENT_LOOP_PAUSED: "paused",
    lib.AGENT_LOOP_MAX_TURNS: "max_turns",
}

# What the loop itself writes for a call it declined to dispatch. Both come from agent_core.h, so
# the model reads the same vocabulary here as it does from the C frontends.
_NOT_RUN = {
    lib.AGENT_LOOP_TOOL_SKIP: (lib.INTERRUPT_MARKER, lib.ITEM_ORIGIN_SKIPPED),
    lib.AGENT_LOOP_TOOL_REFUSE: (lib.REFUSED_RESULT, lib.ITEM_ORIGIN_REFUSED),
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
        agent._diagnostics.append((int(level), _text(message)))


@ffi.def_extern()
def hax_py_checkpoint(user) -> int:
    """Report what should happen at the next seam, from every producer of that answer.

    The cancel flags are process-wide and any thread may set them, so a host calling cancel()
    while send() blocks is answered here. Abort latches pause too, so it is tested first.
    """
    agent = ffi.from_handle(user)
    if agent._pending_exc or lib.cancel_abort_requested():
        return lib.AGENT_LOOP_SIG_ABORT
    if lib.cancel_pause_requested():
        return lib.AGENT_LOOP_SIG_PAUSE
    return lib.AGENT_LOOP_SIG_NONE


@ffi.def_extern()
def hax_py_tick(user) -> int:
    """Stop an in-flight transfer. Without this a cancel would wait out the whole response.

    Also serves compaction's is_cancelled hook, which asks the same question at a different
    moment: a summary streamed through a cancel must not be kept.
    """
    return 1 if lib.cancel_abort_requested() or lib.cancel_pause_requested() else 0


@ffi.def_extern()
def hax_py_compact(user) -> None:
    ffi.from_handle(user).compact()


@ffi.def_extern()
def hax_py_tool_call(call, action, image_input, user):
    """Return an owned result on every path: a call the loop cannot pair corrupts history."""
    agent = ffi.from_handle(user)
    try:
        if action != lib.AGENT_LOOP_TOOL_RUN:
            # The action, not the presentation path, decides what an undispatched call says and
            # how it is marked, so history reads the same whichever frontend answered it.
            output, origin = _NOT_RUN.get(action, _NOT_RUN[lib.AGENT_LOOP_TOOL_SKIP])
            result = lib.agent_tool_result_make(call, output, ffi.NULL)
            result.origin = origin
            return result

        fn = agent._tools.get(_text(call.tool_name))
        if fn is None:
            return agent._run_builtin(call, image_input)

        try:
            arguments = _parse_arguments(_text(call.tool_arguments_json))
            _check_arguments(fn, arguments)
        except HaxError as exc:
            # The model got the call wrong, which it can fix on the next turn. Ending the run
            # with an exception the host cannot act on would throw away a recoverable mistake.
            return lib.agent_tool_result_make(call, f"error: {exc}".encode(), ffi.NULL)

        output = fn(**arguments)
        text = "" if output is None else _stringify(output)
        return lib.agent_tool_result_make(call, text.encode(), ffi.NULL)
    except BaseException:
        # A Python exception cannot unwind through agent_loop_run. Stash it, ask the loop to stop,
        # and still hand back a well-formed result.
        agent._pending_exc = sys.exc_info()
        lib.cancel_request_abort()
        return lib.agent_tool_result_make(call, b"error: the host tool raised an exception",
                                          ffi.NULL)


def _parse_arguments(raw: str) -> dict[str, Any]:
    """Turn the model's argument JSON into keywords, rejecting what cannot be keywords.

    A model can emit anything here, and the failure has to reach the host as a tool error rather
    than as a TypeError from the unpacking, which would read as a bug in the host's own function.
    """
    if not raw.strip():
        return {}
    try:
        parsed = json.loads(raw)
    except ValueError as exc:
        raise HaxError(f"the model sent arguments that are not JSON: {exc}") from exc
    if parsed is None:
        return {}
    if not isinstance(parsed, dict):
        raise HaxError(
            f"the model sent {type(parsed).__name__} arguments; a tool call needs a JSON object"
        )
    return parsed


def _check_arguments(fn: Callable[..., Any], arguments: dict[str, Any]) -> None:
    """Reject a call the function cannot accept, before it becomes a TypeError from inside it.

    A model that invents or omits an argument is making a recoverable mistake; a TypeError
    raised from the host's own body is a bug the host wants to see. Binding against the
    signature first keeps the two apart, so only the second ends the turn.
    """
    try:
        signature = inspect.signature(fn)
    except (TypeError, ValueError):
        return  # No introspectable signature; let the call itself decide.
    try:
        signature.bind(**arguments)
    except TypeError as exc:
        raise HaxError(f"{fn.__name__} cannot accept these arguments: {exc}") from exc


def _stringify(output: Any) -> str:
    """Render a tool's return value for the model.

    dict and list go out as JSON rather than as Python reprs: a model reading `{'a': 1}` with
    single quotes and `None` cannot parse it, and returning structured data is the common case.
    """
    if isinstance(output, str):
        return output
    if isinstance(output, (dict, list, tuple)):
        try:
            return json.dumps(output, default=str)
        except (TypeError, ValueError):
            return str(output)
    return str(output)


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
        self._initialized = False
        self._provider = ffi.NULL
        self._session = None
        # Snapshot taken at close() so the conversation stays readable after the context manager
        # exits, which is when callers usually want to inspect it.
        self._final_items: list[dict[str, Any]] | None = None
        self._diagnostics: list[tuple[int, str]] = []
        self._pending_exc = None
        self._compactions = 0
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
            self._initialized = True

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
                raise HaxError(self._last_diagnostic("could not create a provider"))

            self._session = ffi.new("struct agent_session *")
            opts = ffi.new("struct hax_opts *", {"raw": 0, "resume_path": ffi.NULL,
                                                 "provider_autoselected": 0})
            lib.agent_session_init(self._session, self._provider, opts)
        except BaseException:
            # hax_init() refuses a second call, so a half-built Agent that keeps the process
            # initialized poisons every later one. Release whatever was acquired, in reverse.
            if self._provider != ffi.NULL:
                lib.hax_provider_destroy(self._provider)
                self._provider = ffi.NULL
            if self._initialized:
                lib.hax_shutdown()
                self._initialized = False
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
        # The cancel flags are latched and process-wide; a cancel() that was never consumed must
        # not greet the next Agent in this process.
        lib.cancel_clear_requests()
        if self._initialized:
            lib.hax_shutdown()
            self._initialized = False
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
        """Prefer the most recent error: a warning logged after it rarely explains the failure."""
        for level, message in reversed(self._diagnostics):
            if level == lib.HAX_DIAG_ERR:
                return message
        return self._diagnostics[-1][1] if self._diagnostics else fallback

    # --- running ---

    def cancel(self) -> None:
        """Ask a running send() to stop, raising HaxCancelled in the thread that called it.

        Safe from another thread: the GIL is released for the duration of the loop, and the
        cancel flags are process-wide and atomic. A cancel with no turn running is latched and
        consumed by the next send(), which clears the flags before it starts.
        """
        lib.cancel_request_abort()

    def compact(self) -> bool:
        """Summarize the conversation in place and return whether a summary was appended.

        Compaction appends a seed rather than deleting history, so items still reports every
        turn; the model-visible window is what shrinks. The loop calls this on its own once the
        context crosses hax's configured threshold.
        """
        if self._closed:
            raise HaxError("this Agent is closed")

        params = ffi.new(
            "struct compact_params *",
            {
                "session": self._session,
                "provider": self._provider,
                "session_log": ffi.NULL,
                "transcript_log": ffi.NULL,
                "instructions": ffi.NULL,
                "hooks": {
                    "user": self._handle,
                    "tick": lib.hax_py_tick,
                    "is_cancelled": lib.hax_py_tick,
                },
            },
        )
        result = ffi.new("struct compact_result *")
        lib.compact_run(params, result)
        outcome = result.outcome
        error = _text(result.error_message)
        lib.compact_result_destroy(result)

        if outcome == lib.COMPACT_COMPLETE:
            self._compactions += 1
            return True
        # A failed compaction is not fatal: the turn continues on the uncompacted context and
        # fails at the provider if it really is too large. Say so rather than failing silently.
        self._diagnostics.append(
            (lib.HAX_DIAG_WARN, error or "context compaction did not complete")
        )
        return False

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
                    "tick": lib.hax_py_tick,
                    "checkpoint": lib.hax_py_checkpoint,
                    "tool_call": lib.hax_py_tool_call,
                    "compact": lib.hax_py_compact,
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
        if outcome in (lib.AGENT_LOOP_INTERRUPTED, lib.AGENT_LOOP_PAUSED):
            # History is repaired and fully paired either way, so the conversation stays usable
            # and a later send() continues from a clean seam.
            raise HaxCancelled(f"the turn was cancelled ({_OUTCOMES.get(outcome, outcome)})")
        if outcome != lib.AGENT_LOOP_COMPLETE and outcome != lib.AGENT_LOOP_MAX_TURNS:
            raise HaxError(f"turn ended {_OUTCOMES.get(outcome, outcome)}")

        # Read the assistant text straight off the log: rebuilding every item dict to keep a
        # handful of strings is work the caller did not ask for.
        texts = []
        for i in range(before, self._session.n_items):
            item = self._session.items[i]
            if item.kind == lib.ITEM_ASSISTANT_MESSAGE:
                text = _text(item.text)
                if text:
                    texts.append(text)
        return "\n".join(texts)

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
                    "origin": _ORIGINS.get(item.origin, item.origin),
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
        return [message for _, message in self._diagnostics]

    @property
    def compactions(self) -> int:
        """How many times the context has been summarized during this conversation."""
        return self._compactions

    @property
    def model(self) -> str:
        if self._closed:
            raise HaxError("this Agent is closed")
        return _text(self._session.model)
