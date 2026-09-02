#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Bench-host witness: what actually touched the hardware, and when.

Why this exists separately from the lock
----------------------------------------
``scripts/hil/bench.sh`` and its journal are the lock's OWN bookkeeping. Asking
the journal whether the lock worked is asking the defendant to testify. An
actor that silently skipped its work looks, in the journal, exactly like one
that waited politely and then did it -- both produce one ``acquire`` and one
``release``.

This file is the independent witness. It runs ON the bench host, samples
``/proc``, and answers a question the lock cannot influence: **which process
had the J-Link open, from which machine, between which two instants**. It takes
no lock, opens no device and drives nothing; it only reads ``/proc``.

Attribution without trusting anybody
------------------------------------
Every hardware tool on this bench is reached over ssh, so every such process is
a descendant of an ``sshd`` session whose shell carries ``SSH_CONNECTION`` in
its environment. Walking the ppid chain to that variable yields the PEER
ADDRESS of the machine that started the tool -- read out of the kernel, not
declared by the actor. Two tool processes with different peers alive at once is
a direct observation of two machines on the board at the same time.

Start instants are EXACT, not sampled: ``/proc/<pid>/stat`` field 22 is the
process start in clock ticks since boot, and ``/proc/stat``'s ``btime`` turns
that into a wall-clock instant on the bench host's clock -- the same clock the
journal's timestamps come from. End instants are bracketed by the last sample
that saw the process and the first that did not, so the analyser can widen
every interval to its pessimistic bound and still conclude non-overlap.

Shipped BY VALUE
----------------
The client base64s this file onto an ssh command line, exactly the way
``lib/bench_client.sh`` ships ``lib/bench_host.sh``. The bench Pi's copy of
this tree is whatever a suite last left there; evidence must not depend on it.

Usage::

    bench_witness.py run --out FILE [--interval-ms 50] [--max-seconds 3600]
    bench_witness.py sample                 # one census to stdout, for eyeballs
    bench_witness.py --selftest             # prove discovery both ways, offline

``run`` stops when ``FILE.stop`` appears or ``--max-seconds`` elapses, so a
witness can never outlive the run that started it.

Every scan takes its procfs root as an argument rather than reaching for
``/proc`` directly. That is what lets ``--selftest`` point the scanner at a
SYNTHETIC procfs holding one planted tool and one planted decoy, and require it
to find exactly one -- on any host, including a Mac with no ``/proc`` at all. A
witness that had quietly stopped matching would otherwise report an empty
timeline, and an empty timeline reads as "nothing overlapped".

Exit 0 on a clean run, 1 on a failing selftest, 2 when /proc is not readable.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import time
from pathlib import Path

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

DEFAULT_PROC = Path("/proc")
DEFAULT_LOCK_DIR = Path("/var/lib/ra8-bench")

# Every program on this bench that drives the target, the C6, the hub or the
# plug. Matched against the process COMM first (one cheap read), then against
# the full command line for the interpreted ones, whose comm is `python3`.
TOOL_COMMS = frozenset(
    {
        "JLinkExe",
        "JLinkGDBServer",
        "JLinkGDBServerCLExe",
        "JLinkRTTClient",
        "JLinkRTTLogger",
        "rfp-cli",
        "openocd",
        "uhubctl",
        "esptool",
        "esptool.py",
    }
)

# Command-line substrings that identify a tool whose comm is an interpreter.
TOOL_CMDLINE_MARKS = ("esptool", "tapo_control.py")

# Interpreters worth reading a cmdline for. Anything else is judged on comm.
INTERPRETER_COMMS = frozenset({"python3", "python", "python3.11", "python3.12"})

# The J-Link commander script is passed as a path; its CONTENTS name the hex
# being programmed, and each actor in a contention run programs a different
# app. That is a second attribution axis alongside the ssh peer -- one from the
# kernel, one from the payload -- so neither is taken on trust alone.
SCRIPT_FLAGS = ("-commanderscript", "-CommanderScript", "-commandfile")

# How often the far more expensive /proc/*/fd scan runs, as a divisor of the
# sample rate. Console ownership changes on a human timescale; the J-Link
# census is what needs 20 Hz.
FD_SCAN_EVERY = 20

# Bound on the ppid walk. A corrupted or racing /proc must not loop forever.
PPID_WALK_LIMIT = 64

# /proc/<pid>/stat, split after the last ')', starts at field 3. starttime is
# field 22 of the whole line, so index 19 here, and a shorter split is a line
# this parser does not understand.
STAT_MIN_FIELDS = 20
STAT_STARTTIME_IDX = 19
STAT_PPID_IDX = 1

# The selftest's synthetic procfs: one planted tool, its parent shell carrying
# the ssh peer, and a boot time that makes the expected start instant exact.
FAKE_BTIME = 1700000000.0
FAKE_CLK_TCK = 100.0
TOOL_PID = 111
PARENT_PID = 110
TOOL_TICKS = 250
EPSILON_S = 1e-6


def _read_text(path: Path) -> str:
    """Read *path*, returning '' for anything that vanished or is forbidden."""
    try:
        return path.read_text(errors="replace")
    except (OSError, ValueError):
        return ""


def _read_bytes(path: Path) -> bytes:
    """Read *path* as bytes, returning b'' for anything unreadable."""
    try:
        return path.read_bytes()
    except (OSError, ValueError):
        return b""


def boot_time_epoch(root: Path) -> float:
    """Wall-clock instant of the last boot, from *root*/stat's ``btime``."""
    for line in _read_text(root / "stat").splitlines():
        if line.startswith("btime "):
            return float(line.split()[1])
    return 0.0


def parse_stat(raw: str) -> tuple[int, int]:
    """Return ``(ppid, starttime_ticks)`` from a /proc/<pid>/stat body.

    Field 2 is the comm in parentheses and may itself contain spaces and
    parentheses, so the split is anchored on the LAST ``)`` rather than on
    whitespace. Returns ``(0, 0)`` when the line is not a usable stat line.
    """
    _, sep, rest = raw.rpartition(")")
    if not sep:
        return (0, 0)
    fields = rest.split()
    # `rest` starts at field 3 (state), so ppid is fields[1] and starttime --
    # field 22 of the whole line -- is fields[19].
    if len(fields) < STAT_MIN_FIELDS:
        return (0, 0)
    try:
        return (int(fields[STAT_PPID_IDX]), int(fields[STAT_STARTTIME_IDX]))
    except ValueError:
        return (0, 0)


def ssh_peer(root: Path, pid: int, cache: dict[int, str]) -> str:
    """Peer address of the ssh session *pid* belongs to, or '' when local.

    Walks the ppid chain looking for ``SSH_CONNECTION`` in a process
    environment. The value's first field is the client address as the KERNEL
    saw it, so an actor cannot misreport where it is calling from. Results are
    memoised per pid, and the walk is bounded.
    """
    chain: list[int] = []
    cur = pid
    found = ""
    for _ in range(PPID_WALK_LIMIT):
        if cur <= 1:
            break
        if cur in cache:
            found = cache[cur]
            break
        chain.append(cur)
        for item in _read_bytes(root / str(cur) / "environ").split(b"\0"):
            if item.startswith(b"SSH_CONNECTION="):
                value = item.split(b"=", 1)[1].decode("ascii", "replace").split()
                found = value[0] if value else ""
                break
        if found:
            break
        cur, _ = parse_stat(_read_text(root / str(cur) / "stat"))
    for node in chain:
        cache[node] = found
    return found


def _cmdline(root: Path, pid: int) -> list[str]:
    """Argv of *pid* as a list; empty when the process is gone or a kthread."""
    raw = _read_bytes(root / str(pid) / "cmdline")
    if not raw:
        return []
    return [part.decode("utf-8", "replace") for part in raw.split(b"\0") if part]


def is_tool(comm: str, argv: list[str]) -> bool:
    """True when (comm, argv) names a program that drives bench hardware."""
    if comm in TOOL_COMMS:
        return True
    if comm not in INTERPRETER_COMMS:
        return False
    joined = " ".join(argv)
    return any(mark in joined for mark in TOOL_CMDLINE_MARKS)


def _script_body(argv: list[str]) -> str:
    """Contents of the J-Link commander script named in *argv*, flattened."""
    for flag in SCRIPT_FLAGS:
        if flag not in argv:
            continue
        idx = argv.index(flag)
        if idx + 1 >= len(argv):
            continue
        return " ".join(_read_text(Path(argv[idx + 1])).split())
    return ""


class Sighting:
    """One tool process, from the instant it started to the instant it left."""

    def __init__(self, pid: int, comm: str, argv: list[str], start: float, peer: str) -> None:
        """Record a tool process seen alive, with its exact start instant."""
        self.pid = pid
        self.comm = comm
        self.argv = argv
        self.start = start
        self.peer = peer
        self.script = _script_body(argv)
        self.last_seen = start

    def as_start_event(self, now: float) -> dict[str, object]:
        """The NDJSON record written when this process is first observed."""
        return {
            "ev": "proc_start",
            "pid": self.pid,
            "tool": self.comm,
            "peer": self.peer,
            "start_epoch": round(self.start, 3),
            "first_seen": round(now, 3),
            "argv": " ".join(self.argv),
            "script": self.script,
        }

    def as_end_event(self, now: float) -> dict[str, object]:
        """The NDJSON record written when this process is first missed."""
        return {
            "ev": "proc_end",
            "pid": self.pid,
            "tool": self.comm,
            "peer": self.peer,
            "start_epoch": round(self.start, 3),
            "last_seen": round(self.last_seen, 3),
            "gone_by": round(now, 3),
        }


def scan_tools(
    root: Path, btime: float, clk_tck: float, peers: dict[int, str]
) -> dict[int, Sighting]:
    """Every live bench-tool process under *root*, with exact start instants."""
    found: dict[int, Sighting] = {}
    try:
        entries = list(root.iterdir())
    except OSError:
        return found
    for entry in entries:
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        comm = _read_text(entry / "comm").strip()
        if not comm:
            continue
        argv = _cmdline(root, pid) if (comm in INTERPRETER_COMMS or comm in TOOL_COMMS) else []
        if not is_tool(comm, argv):
            continue
        _, ticks = parse_stat(_read_text(entry / "stat"))
        start = btime + (ticks / clk_tck) if ticks else 0.0
        found[pid] = Sighting(pid, comm, argv, start, ssh_peer(root, pid, peers))
    return found


def scan_consoles(root: Path, peers: dict[int, str]) -> list[dict[str, object]]:
    """Who currently holds a /dev/ttyACM* open, and from which machine.

    The board console is the J-Link OB's VCOM: a second reader silently halves
    everyone's bytes, which is one of the collisions #497 exists to stop. Cheap
    enough at a fraction of the census rate, and far too expensive at all of it.
    """
    holders: list[dict[str, object]] = []
    try:
        entries = list(root.iterdir())
    except OSError:
        return holders
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            fds = list((entry / "fd").iterdir())
        except OSError:
            continue
        for fd in fds:
            try:
                target = str(fd.readlink())
            except OSError:
                continue
            if not target.startswith("/dev/ttyACM"):
                continue
            holders.append(
                {
                    "dev": target,
                    "pid": int(entry.name),
                    "comm": _read_text(entry / "comm").strip(),
                    "peer": ssh_peer(root, int(entry.name), peers),
                }
            )
    return holders


def lock_snapshot(lock_dir: Path) -> dict[str, str]:
    """The lock's own claim about who holds it -- correlation only, not proof.

    Recorded so a reader can line the two accounts up, and so a DISAGREEMENT
    between them is visible. Nothing in the verdict may rest on it: that is the
    whole reason this file exists.
    """
    raw = _read_text(lock_dir / "holder.json")
    out = {"lock_id": "", "holder_name": ""}
    if not raw:
        return out
    for key in out:
        marker = f'"{key}": "'
        idx = raw.find(marker)
        if idx < 0:
            continue
        rest = raw[idx + len(marker) :]
        out[key] = rest[: rest.find('"')] if '"' in rest else ""
    return out


class Witness:
    """The sampling loop and its NDJSON output stream."""

    def __init__(self, out: Path, interval_s: float, root: Path, lock_dir: Path) -> None:
        """Open the NDJSON stream and read the clock constants once."""
        self.out = out
        self.interval_s = interval_s
        self.root = root
        self.lock_dir = lock_dir
        self.btime = boot_time_epoch(root)
        self.clk_tck = float(os.sysconf("SC_CLK_TCK"))
        self.peers: dict[int, str] = {}
        self.live: dict[int, Sighting] = {}
        self.last_census = ""
        self.handle = out.open("a", buffering=1)

    def emit(self, record: dict[str, object]) -> None:
        """Append one NDJSON record, timestamped on the bench host's clock."""
        record.setdefault("t", round(time.time(), 3))
        self.handle.write(json.dumps(record, sort_keys=True) + "\n")

    def census(self, now: float, seen: dict[int, Sighting]) -> None:
        """Emit the concurrently-alive set whenever it changes.

        This is the direct observation of overlap: a single record naming two
        pids with two different peers is two machines on the board at once, and
        needs no interval arithmetic to interpret.
        """
        rows = sorted((s.pid, s.comm, s.peer) for s in seen.values())
        key = repr(rows)
        if key == self.last_census:
            return
        self.last_census = key
        lock = lock_snapshot(self.lock_dir)
        self.emit(
            {
                "ev": "census",
                "t": round(now, 3),
                "tools": [list(row) for row in rows],
                "peers": sorted({s.peer for s in seen.values() if s.peer}),
                "lock_id": lock["lock_id"],
                "lock_holder": lock["holder_name"],
            }
        )

    def step(self, now: float, fd_scan: bool) -> None:
        """One sampling tick: diff the live set, then emit what changed."""
        seen = scan_tools(self.root, self.btime, self.clk_tck, self.peers)
        for pid, sighting in seen.items():
            if pid not in self.live:
                self.live[pid] = sighting
                self.emit(sighting.as_start_event(now))
            self.live[pid].last_seen = now
        for pid in [p for p in self.live if p not in seen]:
            self.emit(self.live.pop(pid).as_end_event(now))
        self.census(now, seen)
        if fd_scan:
            for holder in scan_consoles(self.root, self.peers):
                holder["ev"] = "tty"
                self.emit(holder)

    def run(self, max_seconds: float) -> int:
        """Sample until the stop file appears or *max_seconds* elapses."""
        stop = Path(str(self.out) + ".stop")
        started = time.time()
        self.emit(
            {
                "ev": "witness_start",
                "boot_id": _read_text(self.root / "sys/kernel/random/boot_id").strip(),
                "btime": self.btime,
                "clk_tck": self.clk_tck,
                "interval_s": self.interval_s,
                "host": os.uname().nodename,
            }
        )
        tick = 0
        while not stop.exists() and (time.time() - started) < max_seconds:
            self.step(time.time(), tick % FD_SCAN_EVERY == 0)
            tick += 1
            time.sleep(self.interval_s)
        now = time.time()
        for pid in list(self.live):
            self.emit(self.live.pop(pid).as_end_event(now))
        self.emit({"ev": "witness_stop", "ticks": tick})
        self.handle.close()
        return EXIT_OK


def _plant(root: Path, spec: tuple[int, str, list[str], int, int]) -> None:
    """Write a synthetic /proc/<pid> for the selftest's fake procfs.

    *spec* is ``(pid, comm, argv, ppid, starttime_ticks)`` -- one tuple rather
    than five positional arguments, because five in a row at a call site is
    where a transposed pair hides.
    """
    pid, comm, argv, ppid, ticks = spec
    d = root / str(pid)
    d.mkdir(parents=True, exist_ok=True)
    (d / "comm").write_text(comm + "\n")
    (d / "cmdline").write_bytes(b"\0".join(a.encode() for a in argv) + b"\0")
    filler = " ".join("0" for _ in range(17))
    (d / "stat").write_text(f"{pid} ({comm}) S {ppid} {filler} {ticks}\n")


def _selftest_fake_procfs(tmp: Path) -> list[str]:
    """Plant a tool and two decoys, and require discovery to find only the tool."""
    failures: list[str] = []
    root = tmp / "proc"
    root.mkdir()
    (root / "stat").write_text(f"cpu 1 2 3\nbtime {FAKE_BTIME:.0f}\nprocesses 42\n")
    _plant(root, (TOOL_PID, "JLinkExe", ["JLinkExe", "-nogui", "1"], PARENT_PID, TOOL_TICKS))
    # A decoy that MENTIONS the tool without being it, and one unrelated
    # daemon. A substring matcher would take the first; a comm matcher must not.
    _plant(root, (222, "bash", ["bash", "-c", "echo JLinkExe would run here"], PARENT_PID, 260))
    _plant(root, (333, "sshd", ["sshd:", "star@notty"], 1, 270))
    # The peer must come from an ANCESTOR's environ, not the tool's own.
    (root / str(PARENT_PID)).mkdir()
    (root / str(PARENT_PID) / "comm").write_text("bash\n")
    (root / str(PARENT_PID) / "stat").write_text(
        f"{PARENT_PID} (bash) S 1 " + " ".join("0" for _ in range(17)) + " 240\n"
    )
    (root / str(PARENT_PID) / "environ").write_bytes(
        b"HOME=/home/star\0SSH_CONNECTION=10.0.40.103 5 10.0.40.101 22\0"
    )

    btime = boot_time_epoch(root)
    if btime != FAKE_BTIME:
        failures.append(f"boot_time_epoch on a synthetic procfs: got {btime}")
    tools = scan_tools(root, btime, FAKE_CLK_TCK, {})
    if sorted(tools) != [TOOL_PID]:
        failures.append(f"scan_tools should find exactly pid {TOOL_PID}, found {sorted(tools)}")
    if TOOL_PID in tools:
        sighting = tools[TOOL_PID]
        if sighting.peer != "10.0.40.103":
            failures.append(f"peer must come from the ancestor environ, got '{sighting.peer}'")
        want = FAKE_BTIME + TOOL_TICKS / FAKE_CLK_TCK
        if abs(sighting.start - want) > EPSILON_S:
            failures.append(f"exact start instant: want {want}, got {sighting.start}")
    return failures


def selftest() -> int:
    """Prove discovery and parsing in BOTH directions, on any host.

    This repo's dominant tooling defect is a checker that quietly stopped
    matching and reported a clean tree forever. A witness in that state emits an
    empty timeline, and an empty timeline reads as "no two actors ever
    overlapped" -- a green verdict manufactured by a broken tool. So the scanner
    is pointed at a synthetic procfs holding one real tool and two decoys and
    required to find exactly the one, and the stat parser is required to reject
    the lines it cannot understand rather than returning zeros.
    """
    failures: list[str] = []

    # A comm containing spaces AND parentheses is the case a whitespace split
    # gets wrong; JLinkExe's is tame, but a wrapper's need not be.
    line = "42 (weird ) name) S 7 " + " ".join("0" for _ in range(17)) + " 99887766"
    got = parse_stat(line)
    if got != (7, 99887766):
        failures.append(f"parse_stat on a parenthesised comm: want (7, 99887766), got {got}")
    if parse_stat("not a stat line") != (0, 0):
        failures.append("parse_stat accepted a non-stat line")
    if parse_stat("1 (init) S 0") != (0, 0):
        failures.append("parse_stat accepted a truncated stat line")

    if not is_tool("JLinkExe", []):
        failures.append("is_tool missed a bare JLinkExe")
    if not is_tool("python3", ["python3", "/x/esptool.py", "flash"]):
        failures.append("is_tool missed esptool under an interpreter")
    if is_tool("bash", ["bash", "-c", "echo JLinkExe"]):
        failures.append("is_tool matched a mention of JLinkExe rather than an invocation")
    if is_tool("sshd", []):
        failures.append("is_tool matched sshd")

    with tempfile.TemporaryDirectory() as tmp:
        failures.extend(_selftest_fake_procfs(Path(tmp)))

    for text in failures:
        print(f"bench_witness: SELFTEST FAIL -- {text}", file=sys.stderr)
    if failures:
        return EXIT_FAIL
    print("bench_witness: selftest OK -- parser and discovery proven both ways")
    return EXIT_OK


def _cmd_sample(args: argparse.Namespace) -> int:
    """One census to stdout, for a human looking at a live bench."""
    peers: dict[int, str] = {}
    btime = boot_time_epoch(args.proc)
    tools = scan_tools(args.proc, btime, float(os.sysconf("SC_CLK_TCK")), peers)
    print(
        json.dumps(
            {
                "tools": [t.as_start_event(time.time()) for t in tools.values()],
                "consoles": scan_consoles(args.proc, peers),
                "lock": lock_snapshot(args.lock_dir),
            },
            indent=2,
            sort_keys=True,
        )
    )
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Parse arguments and dispatch. See the module docstring for the verbs."""
    parser = argparse.ArgumentParser(description="bench-host hardware witness")
    parser.add_argument("verb", nargs="?", default="sample", choices=("run", "sample"))
    parser.add_argument("--out", type=Path, help="NDJSON output path (run)")
    parser.add_argument("--interval-ms", type=int, default=50)
    parser.add_argument("--max-seconds", type=float, default=3600.0)
    parser.add_argument("--proc", type=Path, default=DEFAULT_PROC)
    parser.add_argument("--lock-dir", type=Path, default=DEFAULT_LOCK_DIR)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.proc.is_dir():
        print(
            f"bench_witness: {args.proc} is not readable -- not a Linux bench host", file=sys.stderr
        )
        return EXIT_CONFIG
    if args.verb == "sample":
        return _cmd_sample(args)
    if args.out is None:
        print("bench_witness: run needs --out", file=sys.stderr)
        return EXIT_CONFIG
    return Witness(args.out, args.interval_ms / 1000.0, args.proc, args.lock_dir).run(
        args.max_seconds
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
