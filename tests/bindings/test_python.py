#!/usr/bin/env python3
"""The cffi binding drives a full user turn, including host-defined tools.

Registered only when -Dembed=true builds libhax and the cffi extension. Otherwise hermetic, like
the e2e scenarios.
"""

import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MOCK_DIR = REPO_ROOT / "scripts" / "mock"

failures = 0


def expect(condition: bool, description: str) -> None:
    global failures
    if not condition:
        print(f"FAIL: {description}", file=sys.stderr)
        failures += 1


def use_mock(script: str) -> None:
    os.environ["HAX_PROVIDER"] = "mock"
    os.environ["HAX_MOCK_SCRIPT"] = str(MOCK_DIR / script)


# A scratch HOME keeps the developer's real config and sessions out of the run.
scratch = Path(os.environ.get("MESON_BUILD_ROOT", "/tmp")) / "hax-binding-scratch"
scratch.mkdir(parents=True, exist_ok=True)
os.environ["HOME"] = str(scratch)
os.environ["HAX_KEEP_AWAKE"] = "0"
for key in [k for k in os.environ if k.startswith("XDG_")]:
    del os.environ[key]

# Meson builds the extension; HAX_EXTENSION_DIR points at the one under test.
sys.path.insert(0, str(REPO_ROOT / "bindings" / "python"))
try:
    import hax
except ImportError as exc:
    print(f"SKIP: the cffi extension is not built ({exc})", file=sys.stderr)
    sys.exit(77)


def test_plain_turn() -> None:
    use_mock("hello.txt")
    with hax.Agent(provider="mock") as agent:
        expect(agent.model == "mock-model", "model resolves from the mock provider")
        reply = agent.send("hello")
        expect(bool(reply), f"a plain turn returns assistant text (got {reply!r})")
        kinds = [item["kind"] for item in agent.items]
        expect("user" in kinds and "assistant" in kinds, f"history records the turn: {kinds}")


def test_host_tool_runs() -> None:
    use_mock("python_tool.txt")
    seen = []
    with hax.Agent(provider="mock") as agent:

        @agent.tool
        def lookup_order(order_id):
            seen.append(order_id)
            return "two widgets"

        agent.send("order 4417?")
        expect(seen == ["4417"], f"host tool receives parsed arguments (got {seen})")
        outputs = [i["output"] for i in agent.items if i["kind"] == "tool_result"]
        expect(outputs == ["two widgets"], f"tool output reaches history (got {outputs})")


def test_builtin_tool_still_runs() -> None:
    use_mock("tool_roundtrip.txt")
    workdir = scratch / "work"
    workdir.mkdir(exist_ok=True)
    marker = workdir / "out.txt"
    if marker.exists():
        marker.unlink()
    previous = Path.cwd()
    os.chdir(workdir)
    try:
        with hax.Agent(provider="mock") as agent:
            agent.send("go")
    finally:
        os.chdir(previous)
    expect(marker.exists(), "an unshadowed call still runs hax's own bash tool")
    if marker.exists():
        expect(marker.read_text() == "marker42\n", "built-in tool produced the scripted content")


def test_host_exception_propagates_and_history_stays_paired() -> None:
    use_mock("python_tool.txt")
    with hax.Agent(provider="mock") as agent:

        @agent.tool
        def lookup_order(order_id):
            raise ValueError("database is down")

        raised = None
        try:
            agent.send("order 4417?")
        except ValueError as exc:
            raised = exc
        expect(raised is not None, "the host exception reaches the caller")
        expect(str(raised) == "database is down", f"the original exception survives ({raised})")

        kinds = [i["kind"] for i in agent.items]
        expect(
            kinds.count("tool_call") == kinds.count("tool_result"),
            f"every call is paired with a result after an aborted turn: {kinds}",
        )


def test_database_example_enforces_read_only() -> None:
    """The shipped example's guard must actually reject a write, not rely on the prompt."""
    use_mock("database_agent.txt")
    sys.path.insert(0, str(REPO_ROOT / "bindings" / "python"))
    from example_database import build_demo_database, register_tools

    connection = build_demo_database()
    try:
        with hax.Agent(provider="mock") as agent:
            executed = register_tools(agent, connection)
            agent.send("which customer spent the most?")

        # items stays readable after close, which is where callers usually inspect it.
        outputs = [i["output"] for i in agent.items if i["kind"] == "tool_result"]
        expect(
            any("only SELECT and WITH queries are allowed" in o for o in outputs),
            f"a write is refused with a recoverable error (got {outputs})",
        )
        expect(
            all("drop" not in statement.lower() for statement in executed),
            f"the refused statement never reached sqlite (log: {executed})",
        )
        survived = connection.execute(
            "SELECT name FROM sqlite_master WHERE name = 'orders'"
        ).fetchone()
        expect(survived is not None, "the table the model tried to drop is still there")
    finally:
        connection.close()


def test_second_agent_is_refused() -> None:
    use_mock("hello.txt")
    with hax.Agent(provider="mock"):
        refused = False
        try:
            hax.Agent(provider="mock")
        except hax.HaxError:
            refused = True
        expect(refused, "a second Agent is refused while one is live")
    # The guard releases on close, so a later Agent works.
    with hax.Agent(provider="mock") as agent:
        expect(agent.model == "mock-model", "an Agent can be created after the first closes")


def test_unknown_provider_reports_a_diagnostic() -> None:
    use_mock("hello.txt")
    raised = None
    try:
        hax.Agent(provider="no-such-provider")
    except hax.HaxError as exc:
        raised = exc
    expect(raised is not None, "an unknown provider raises")
    expect("no-such-provider" in str(raised), f"the diagnostic names the provider ({raised})")


test_plain_turn()
test_host_tool_runs()
test_builtin_tool_still_runs()
test_host_exception_propagates_and_history_stays_paired()
test_database_example_enforces_read_only()
test_second_agent_is_refused()
test_unknown_provider_reports_a_diagnostic()

sys.exit(1 if failures else 0)
