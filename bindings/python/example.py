#!/usr/bin/env python3
"""Run one turn through libhax with a host-defined tool.

    meson setup build-embed -Dembed=true && meson compile -C build-embed
    uv run python bindings/python/hax_build.py
    HAX_PROVIDER=mock HAX_MOCK_SCRIPT=scripts/mock/python_tool.txt \
        uv run python bindings/python/example.py

Drop the mock variables and pass a real provider to talk to a live model.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hax

ORDERS = {"4417": "two widgets and a length of rope"}

with hax.Agent(provider="mock") as agent:

    @agent.tool
    def lookup_order(order_id):
        """Return the contents of an order."""
        return ORDERS.get(order_id, "no such order")

    print(agent.send("what is in order 4417?"))

    print("\n--- conversation ---")
    for item in agent.items:
        if item["kind"] == "tool_call":
            print(f"  call   {item['tool_name']}({item['arguments']})")
        elif item["kind"] == "tool_result":
            print(f"  result {item['output']}")
        elif item["text"]:
            print(f"  {item['kind']:<9} {item['text']}")
