#!/usr/bin/env python3
"""Answer questions about a SQLite database by giving the model read-only query tools.

This is the case a subprocess cannot cover. `hax -p` can run a shell command, but it cannot hand
the model a live connection, enforce a policy on every statement, or keep per-call state in the
host process. Here the tools close over one open connection and refuse anything but a SELECT.

    meson setup build-embed -Dembed=true && meson compile -C build-embed

    export ANTHROPIC_API_KEY=...
    uv run python bindings/python/example_database.py --provider anthropic \
        --model claude-sonnet-5 "which customer spent the most, and on what?"

Any configured provider works; `--provider mock` with HAX_MOCK_SCRIPT replays a fixture instead
of calling a model.
"""

from __future__ import annotations

import argparse
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hax

SCHEMA = """
CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT NOT NULL, city TEXT NOT NULL);
CREATE TABLE orders (
    id INTEGER PRIMARY KEY,
    customer_id INTEGER NOT NULL REFERENCES customers(id),
    item TEXT NOT NULL,
    cents INTEGER NOT NULL,
    placed_on TEXT NOT NULL
);
INSERT INTO customers VALUES
    (1, 'Ada Lovelace', 'London'),
    (2, 'Grace Hopper', 'New York'),
    (3, 'Karen Sparck Jones', 'Cambridge');
INSERT INTO orders VALUES
    (1, 1, 'analytical engine parts', 450000, '2026-03-02'),
    (2, 2, 'compiler manual',           2900, '2026-03-11'),
    (3, 1, 'punch card stock',          8100, '2026-04-01'),
    (4, 3, 'retrieval corpus licence', 125000, '2026-04-17'),
    (5, 2, 'nanosecond wire',            1200, '2026-05-05');
"""

MAX_ROWS = 50


def build_demo_database() -> sqlite3.Connection:
    connection = sqlite3.connect(":memory:")
    connection.executescript(SCHEMA)
    connection.row_factory = sqlite3.Row
    return connection


def register_tools(agent: hax.Agent, connection: sqlite3.Connection) -> list[str]:
    """Give the agent read-only access to `connection`. Returns the log of executed statements."""
    executed: list[str] = []

    @agent.tool
    def describe_schema():
        """List the tables and their columns."""
        rows = connection.execute(
            "SELECT sql FROM sqlite_master WHERE type = 'table' ORDER BY name"
        ).fetchall()
        return "\n".join(row["sql"] for row in rows)

    @agent.tool
    def run_query(sql):
        """Run one read-only SELECT and return its rows."""
        # The model is not a trust boundary. Enforce read-only here, where the connection is,
        # rather than hoping the prompt holds.
        statement = sql.strip().rstrip(";")
        if ";" in statement:
            return "error: one statement per call"
        if not statement.lower().startswith(("select", "with")):
            return "error: only SELECT and WITH queries are allowed"

        executed.append(statement)
        try:
            rows = connection.execute(statement).fetchmany(MAX_ROWS)
        except sqlite3.Error as exc:
            # A recoverable error: returning it lets the model correct its own SQL.
            return f"error: {exc}"
        if not rows:
            return "(no rows)"

        header = " | ".join(rows[0].keys())
        body = "\n".join(" | ".join(str(value) for value in row) for row in rows)
        return f"{header}\n{body}"

    return executed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("question", nargs="?",
                        default="which customer spent the most, and on what?")
    parser.add_argument("--provider", default="anthropic")
    parser.add_argument("--model", default=None, help="defaults to the provider's configuration")
    args = parser.parse_args()

    connection = build_demo_database()
    try:
        with hax.Agent(
            provider=args.provider,
            model=args.model,
            system_prompt=(
                "You answer questions about a SQLite database. Inspect the schema before "
                "querying, and base every claim on rows you actually retrieved."
            ),
        ) as agent:
            executed = register_tools(agent, connection)
            print(agent.send(args.question))
            if executed:
                print("\n--- SQL the model ran ---")
                for statement in executed:
                    print(f"  {statement}")
    except hax.HaxProviderError as exc:
        print(f"provider error: {exc}", file=sys.stderr)
        return 1
    except hax.HaxError as exc:
        print(f"hax: {exc}", file=sys.stderr)
        return 1
    finally:
        connection.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
