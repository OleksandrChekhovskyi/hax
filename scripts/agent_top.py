#!/usr/bin/env python3
"""Live footprint monitor for coding agents: private memory and CPU per process tree.

Discovers processes whose executable name matches a watch list (hax and well-known agents by
default) and shows one row per agent: private memory (current and peak), current CPU%, and
cumulative CPU time. Subprocesses spawned by an agent aggregate on an indented ``+N procs``
line so the agent's own footprint stays distinct from its workload; press ``t`` to expand the
full process tree, ``q`` to quit.

Metric notes:

- Private memory is what a process costs beyond shared pages: phys_footprint on macOS (the
  Activity Monitor "Memory" number), the Private_* total from smaps_rollup on Linux.
- Memory peaks are kernel lifetime counters on macOS; on Linux, and for subprocess aggregates
  everywhere, they are maxima of sampled values and can miss spikes between samples.
- CPU time is exact kernel accounting (utime+stime). The subprocess line includes children the
  agent already reaped, so short-lived tool processes count even if never sampled alive.
- An agent nested inside another watched agent — a subagent, or an agent launched from the
  outer agent's session — counts toward the outer agent's subprocess totals like any other
  child. Agents you want to compare should run side by side, not nested.

The monitor excludes itself and its helpers from all numbers, but run it from a separate
terminal when measuring, so its own work does not sit inside the tree of the agent under test.
"""

from __future__ import annotations

import argparse
import ctypes
import math
import os
import select
import signal
import subprocess
import sys
import termios
import time
import tty
from collections import defaultdict
from collections.abc import Callable
from dataclasses import dataclass
from typing import Final

signal.signal(signal.SIGPIPE, signal.SIG_DFL)

DEFAULT_WATCH: Final[tuple[str, ...]] = (
    "hax", "claude", "codex", "opencode", "pi", "llama-server",
)

INTERPRETERS: Final = frozenset({"node", "bun", "deno", "python", "python3", "sh", "bash", "zsh"})

ESC: Final = "\x1b"
ALT_SCREEN_ON: Final = f"{ESC}[?1049h{ESC}[?25l"
ALT_SCREEN_OFF: Final = f"{ESC}[?25h{ESC}[?1049l"


@dataclass(frozen=True)
class Proc:
    pid: int
    ppid: int
    label: str  # executable basename, for display
    names: frozenset[str]  # candidate names matched against the watch list


def match_names(exe: str, argv_names: list[str]) -> frozenset[str]:
    # argv[1] is a candidate only under an interpreter, where it names the script being run
    # (an npm-installed agent, say); anywhere else it is data, and `vim pi` is not the pi agent.
    names = {exe, *argv_names[:1]}
    # macOS CPython execs the framework binary "Python", so compare case-insensitively.
    if {name.lower() for name in names} & INTERPRETERS and len(argv_names) > 1:
        names.add(argv_names[1])
    return frozenset(names)


@dataclass(frozen=True)
class Metrics:
    mem: int  # private bytes
    mem_peak: int  # kernel lifetime peak, 0 where the platform does not track it
    cpu: float  # self utime+stime, seconds
    child_cpu: float  # reaped children's time the kernel rolled into this process


# ---------------------------------------------------------------------------
# macOS backend: ps for topology, proc_pid_rusage(RUSAGE_INFO_V4) for metrics.


class RusageInfoV4(ctypes.Structure):
    # Field order must match struct rusage_info_v4 in <sys/resource.h> exactly.
    _fields_ = [("ri_uuid", ctypes.c_uint8 * 16)] + [
        (name, ctypes.c_uint64)
        for name in (
            "ri_user_time", "ri_system_time", "ri_pkg_idle_wkups", "ri_interrupt_wkups",
            "ri_pageins", "ri_wired_size", "ri_resident_size", "ri_phys_footprint",
            "ri_proc_start_abstime", "ri_proc_exit_abstime", "ri_child_user_time",
            "ri_child_system_time", "ri_child_pkg_idle_wkups", "ri_child_interrupt_wkups",
            "ri_child_pageins", "ri_child_elapsed_abstime", "ri_diskio_bytesread",
            "ri_diskio_byteswritten", "ri_cpu_time_qos_default", "ri_cpu_time_qos_maintenance",
            "ri_cpu_time_qos_background", "ri_cpu_time_qos_utility", "ri_cpu_time_qos_legacy",
            "ri_cpu_time_qos_user_initiated", "ri_cpu_time_qos_user_interactive",
            "ri_billed_system_time", "ri_serviced_system_time", "ri_logical_writes",
            "ri_lifetime_max_phys_footprint", "ri_instructions", "ri_cycles",
            "ri_billed_energy", "ri_serviced_energy", "ri_interval_max_phys_footprint",
            "ri_runnable_time",
        )
    ]


class MachTimebase(ctypes.Structure):
    _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]


def darwin_backend() -> tuple[Callable[[], dict[int, Proc]], Callable[[int], Metrics | None]]:
    libc = ctypes.CDLL(None, use_errno=True)
    timebase = MachTimebase()
    libc.mach_timebase_info(ctypes.byref(timebase))
    # rusage_info times are in mach_absolute_time units, not nanoseconds.
    abstime_sec = timebase.numer / timebase.denom / 1e9

    def ps_lines(columns: str) -> list[str]:
        out = subprocess.run(
            ["ps", "-axww", "-o", columns], capture_output=True, text=True, check=True
        )
        return out.stdout.splitlines()

    def processes() -> dict[int, Proc]:
        argv_names: dict[int, list[str]] = {}
        for line in ps_lines("pid=,command="):
            parts = line.split(None, 1)
            if len(parts) == 2:
                argv_names[int(parts[0])] = [
                    os.path.basename(token) for token in parts[1].split()[:2]
                ]
        procs: dict[int, Proc] = {}
        for line in ps_lines("pid=,ppid=,comm="):
            parts = line.split(None, 2)
            if len(parts) < 3:
                continue
            pid, ppid = int(parts[0]), int(parts[1])
            label = os.path.basename(parts[2])
            procs[pid] = Proc(pid, ppid, label, match_names(label, argv_names.get(pid, [])))
        return procs

    def metrics(pid: int) -> Metrics | None:
        info = RusageInfoV4()
        if libc.proc_pid_rusage(pid, 4, ctypes.byref(info)) != 0:
            return None
        return Metrics(
            info.ri_phys_footprint,
            info.ri_lifetime_max_phys_footprint,
            (info.ri_user_time + info.ri_system_time) * abstime_sec,
            (info.ri_child_user_time + info.ri_child_system_time) * abstime_sec,
        )

    return processes, metrics


# ---------------------------------------------------------------------------
# Linux backend: /proc/<pid>/stat for topology and CPU, smaps_rollup for memory.


def linux_backend() -> tuple[Callable[[], dict[int, Proc]], Callable[[int], Metrics | None]]:
    clk_tck = os.sysconf("SC_CLK_TCK")

    def read_stat(pid: int) -> tuple[str, int, float, float] | None:
        try:
            with open(f"/proc/{pid}/stat") as f:
                data = f.read()
        except OSError:
            return None
        # comm may contain spaces and parentheses; the last ')' ends it.
        comm = data[data.index("(") + 1 : data.rindex(")")]
        fields = data[data.rindex(")") + 2 :].split()
        cpu = (int(fields[11]) + int(fields[12])) / clk_tck
        child_cpu = (int(fields[13]) + int(fields[14])) / clk_tck
        return comm, int(fields[1]), cpu, child_cpu

    def processes() -> dict[int, Proc]:
        procs: dict[int, Proc] = {}
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            pid = int(entry)
            stat = read_stat(pid)
            if stat is None:
                continue
            comm, ppid, _, _ = stat
            argv_names = []
            try:
                with open(f"/proc/{pid}/cmdline", "rb") as f:
                    argv = f.read().split(b"\0")
                argv_names = [
                    os.path.basename(arg.decode(errors="replace")) for arg in argv[:2] if arg
                ]
            except OSError:
                pass
            procs[pid] = Proc(pid, ppid, comm, match_names(comm, argv_names))
        return procs

    def metrics(pid: int) -> Metrics | None:
        stat = read_stat(pid)
        if stat is None:
            return None
        _, _, cpu, child_cpu = stat
        mem = 0
        try:
            with open(f"/proc/{pid}/smaps_rollup") as f:
                for line in f:
                    if line.startswith("Private_"):
                        mem += int(line.split()[1]) * 1024
        except OSError:
            pass
        return Metrics(mem, 0, cpu, child_cpu)

    return processes, metrics


# ---------------------------------------------------------------------------
# Sampling: group processes into agent trees and aggregate metrics.


@dataclass(frozen=True)
class TreeEntry:
    depth: int
    proc: Proc
    mem: int
    mem_peak: int
    cpu_pct: float | None
    cpu_time: float


@dataclass(frozen=True)
class Row:
    name: str
    pid: int
    mem: int
    mem_peak: int
    cpu_pct: float | None
    cpu_time: float
    kids: int
    kids_ever: bool
    kids_mem: int
    kids_mem_peak: int
    kids_cpu_pct: float | None
    kids_cpu_time: float
    tree: tuple[TreeEntry, ...]


@dataclass
class RootState:
    kids_mem_peak: int = 0
    kids_ever: bool = False


class Monitor:
    def __init__(
        self,
        watch: list[str],
        processes: Callable[[], dict[int, Proc]],
        metrics: Callable[[int], Metrics | None],
    ):
        self.watch = watch
        self.processes = processes
        self.metrics = metrics
        self.states: dict[tuple[int, str], RootState] = {}
        self.prev_cpu: dict[tuple[str, int], tuple[float, float]] = {}
        self.mem_peaks: dict[int, int] = {}

    def sample(self) -> list[Row]:
        now = time.monotonic()
        procs = self.processes()
        self._drop_self(procs)
        children: dict[int, list[int]] = defaultdict(list)
        for proc in procs.values():
            if proc.ppid != proc.pid:
                children[proc.ppid].append(proc.pid)

        watched: dict[int, str] = {}
        for proc in procs.values():
            for name in self.watch:
                if name in proc.names:
                    watched[proc.pid] = name
                    break

        # A match nested under another match — a subagent, a router worker — is workload of
        # the outermost agent, so only matches with no watched ancestor become rows.
        roots = {
            pid for pid in watched if self._watched_ancestor(procs, watched, pid) is None
        }
        self._prune_exited(roots, watched)

        memo: dict[int, Metrics] = {}

        def metric(pid: int) -> Metrics:
            if pid not in memo:
                memo[pid] = self.metrics(pid) or Metrics(0, 0, 0.0, 0.0)
            return memo[pid]

        prev_cpu, self.prev_cpu = self.prev_cpu, {}

        def rate(key: tuple[str, int], cpu: float) -> float | None:
            prev = prev_cpu.get(key)
            self.prev_cpu[key] = (now, cpu)
            if prev is None or now <= prev[0]:
                return None
            return max(0.0, (cpu - prev[1]) / (now - prev[0]) * 100)

        prev_peaks, self.mem_peaks = self.mem_peaks, {}

        def peak(pid: int, m: Metrics) -> int:
            # Linux tracks no lifetime footprint peak, so carry a sampled maximum per pid.
            sampled = max(prev_peaks.get(pid, 0), m.mem)
            self.mem_peaks[pid] = sampled
            return max(m.mem_peak, sampled)

        rows = []
        for root in sorted(roots, key=lambda pid: (self.watch.index(watched[pid]), pid)):
            state = self.states.setdefault((root, watched[root]), RootState())
            root_m = metric(root)
            tree = []
            for depth, pid in self._descendants(children, root):
                m = metric(pid)
                tree.append(
                    TreeEntry(depth, procs[pid], m.mem, peak(pid, m), rate(("p", pid), m.cpu),
                              m.cpu)
                )

            kids_mem = sum(e.mem for e in tree)
            # The root's child counters cover reaped children; live descendants carry their
            # own totals plus whatever they reaped themselves, so nothing is double counted.
            kids_cpu = root_m.child_cpu + sum(metric(e.proc.pid).child_cpu for e in tree)
            kids_cpu += sum(e.cpu_time for e in tree)

            state.kids_mem_peak = max(state.kids_mem_peak, kids_mem)
            state.kids_ever = state.kids_ever or bool(tree)

            rows.append(
                Row(
                    name=watched[root],
                    pid=root,
                    mem=root_m.mem,
                    mem_peak=peak(root, root_m),
                    cpu_pct=rate(("p", root), root_m.cpu),
                    cpu_time=root_m.cpu,
                    kids=len(tree),
                    kids_ever=state.kids_ever,
                    kids_mem=kids_mem,
                    kids_mem_peak=state.kids_mem_peak,
                    kids_cpu_pct=rate(("k", root), kids_cpu),
                    kids_cpu_time=kids_cpu,
                    tree=tuple(tree),
                )
            )
        return rows

    def _drop_self(self, procs: dict[int, Proc]) -> None:
        children: dict[int, list[int]] = defaultdict(list)
        for proc in procs.values():
            if proc.ppid != proc.pid:
                children[proc.ppid].append(proc.pid)
        stack = [os.getpid()]
        while stack:
            pid = stack.pop()
            stack.extend(children[pid])
            procs.pop(pid, None)

    def _watched_ancestor(
        self, procs: dict[int, Proc], watched: dict[int, str], pid: int
    ) -> int | None:
        seen = {pid}
        while pid in procs:
            pid = procs[pid].ppid
            if pid in seen:
                return None
            seen.add(pid)
            if pid in watched:
                return pid
        return None

    def _descendants(self, children: dict[int, list[int]], root: int) -> list[tuple[int, int]]:
        out = []
        stack = [(1, pid) for pid in sorted(children[root], reverse=True)]
        while stack:
            depth, pid = stack.pop()
            out.append((depth, pid))
            stack.extend((depth + 1, kid) for kid in sorted(children[pid], reverse=True))
        return out

    def _prune_exited(self, roots: set[int], watched: dict[int, str]) -> None:
        live = {(pid, watched[pid]) for pid in roots}
        for key in [k for k in self.states if k not in live]:
            del self.states[key]


# ---------------------------------------------------------------------------
# Presentation.


def fmt_mem(n: int) -> str:
    if n >= 1 << 30:
        return f"{n / (1 << 30):.1f}G"
    if n >= 1 << 20:
        return f"{n / (1 << 20):.1f}M"
    if n >= 1024:
        return f"{n / 1024:.0f}K"
    return f"{n}B"


def fmt_cpu_time(seconds: float) -> str:
    if seconds < 60:
        return f"{seconds:.1f}s"
    minutes, secs = divmod(int(seconds), 60)
    if minutes < 60:
        return f"{minutes}m{secs:02d}s"
    hours, minutes = divmod(minutes, 60)
    return f"{hours}h{minutes:02d}m"


def fmt_pct(pct: float | None) -> str:
    return "-" if pct is None else f"{pct:.1f}"


@dataclass(frozen=True)
class Style:
    bold: str = ""
    dim: str = ""
    reset: str = ""

    @staticmethod
    def for_tty() -> Style:
        return Style(bold=f"{ESC}[1m", dim=f"{ESC}[2m", reset=f"{ESC}[0m")


def table_line(name: str, pid: str, mem: str, peak: str, pct: str, cpu_time: str) -> str:
    return f"{name:<18.18} {pid:>7} {mem:>8} {peak:>8} {pct:>6} {cpu_time:>8}"


def render(rows: list[Row], watch: list[str], tree_mode: bool, style: Style) -> list[str]:
    header = table_line("NAME", "PID", "MEM", "PEAK", "CPU%", "TIME")
    lines = [f"{style.dim}{header}{style.reset}"]
    if not rows:
        lines.append(f"no matching processes; watching: {' '.join(watch)}")
    for row in rows:
        lines.append(
            style.bold
            + table_line(
                row.name, str(row.pid), fmt_mem(row.mem), fmt_mem(row.mem_peak),
                fmt_pct(row.cpu_pct), fmt_cpu_time(row.cpu_time),
            )
            + style.reset
        )
        if tree_mode:
            for entry in row.tree:
                lines.append(
                    table_line(
                        "  " * entry.depth + entry.proc.label,
                        str(entry.proc.pid), fmt_mem(entry.mem),
                        fmt_mem(entry.mem_peak) if entry.mem_peak else "-",
                        fmt_pct(entry.cpu_pct), fmt_cpu_time(entry.cpu_time),
                    )
                )
        elif row.kids_ever:
            lines.append(
                style.dim
                + table_line(
                    f"  +{row.kids} procs", "", fmt_mem(row.kids_mem),
                    fmt_mem(row.kids_mem_peak), fmt_pct(row.kids_cpu_pct),
                    fmt_cpu_time(row.kids_cpu_time),
                )
                + style.reset
            )
    return lines


def run_plain(monitor: Monitor, args: argparse.Namespace, style: Style) -> None:
    taken = 0
    while True:
        rows = monitor.sample()
        taken += 1
        print(time.strftime("%H:%M:%S"))
        print("\n".join(render(rows, monitor.watch, args.tree, style)))
        print(flush=True)
        if args.samples and taken >= args.samples:
            return
        time.sleep(args.interval)


def run_interactive(monitor: Monitor, args: argparse.Namespace) -> None:
    style = Style.for_tty()
    tree_mode = args.tree
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    sys.stdout.write(ALT_SCREEN_ON)
    tty.setcbreak(fd)
    rows: list[Row] = []
    taken = 0

    def draw() -> None:
        title = (
            f"{style.bold}agent_top{style.reset}  {time.strftime('%H:%M:%S')}"
            f"  every {args.interval:g}s  {style.dim}t: tree  q: quit{style.reset}"
        )
        body = render(rows, monitor.watch, tree_mode, style)
        frame = "\n".join(line + f"{ESC}[K" for line in [title, ""] + body)
        sys.stdout.write(f"{ESC}[H{frame}{ESC}[J")
        sys.stdout.flush()

    try:
        next_at = time.monotonic()
        while True:
            timeout = max(0.0, next_at - time.monotonic())
            ready, _, _ = select.select([fd], [], [], timeout)
            if ready:
                key = os.read(fd, 1)
                if key == b"q":
                    break
                if key == b"t":
                    tree_mode = not tree_mode
                    draw()
                continue
            rows = monitor.sample()
            taken += 1
            draw()
            if args.samples and taken >= args.samples:
                break
            next_at = time.monotonic() + args.interval
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        sys.stdout.write(ALT_SCREEN_OFF)
        sys.stdout.flush()
    print(time.strftime("%H:%M:%S"))
    print("\n".join(render(rows, monitor.watch, tree_mode, style)))


def positive_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return value


def positive_int(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Monitor private memory and CPU of coding agents and their subprocesses.",
        epilog=f"default watch list: {' '.join(DEFAULT_WATCH)}",
    )
    parser.add_argument("names", nargs="*", help="watch these names instead of the default list")
    parser.add_argument(
        "--add", action="append", default=[], metavar="NAME",
        help="watch NAME in addition to the default list",
    )
    parser.add_argument(
        "--interval", type=positive_float, default=1.0, metavar="SEC",
        help="sample interval (default 1.0)",
    )
    parser.add_argument("--tree", action="store_true", help="start with process trees expanded")
    parser.add_argument(
        "--plain", action="store_true", help="print snapshots instead of the live screen"
    )
    parser.add_argument(
        "--samples", type=positive_int, metavar="N",
        help="exit after N samples (useful with --plain)",
    )
    args = parser.parse_args()

    if sys.platform == "darwin":
        processes, metrics = darwin_backend()
    elif sys.platform.startswith("linux"):
        processes, metrics = linux_backend()
    else:
        print(f"unsupported platform: {sys.platform}", file=sys.stderr)
        return 1

    watch: list[str] = list(args.names) if args.names else list(DEFAULT_WATCH)
    watch += [name for name in args.add if name not in watch]
    monitor = Monitor(watch, processes, metrics)

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(130))
    if args.plain or not (sys.stdin.isatty() and sys.stdout.isatty()):
        run_plain(monitor, args, Style.for_tty() if sys.stdout.isatty() else Style())
    else:
        run_interactive(monitor, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
