#!/usr/bin/env python3
"""Runs the channel-count cases on test/example.yaml and checks them.

    python3 test/channels/run.py [--binary build/ramulator2] [--update]

Three kinds of result are kept apart, so that a legitimate timing-model change never reads as a channel
regression:

  FUNCTIONAL   the right channels did the work, and a channel the system lacks is refused
  PERFORMANCE  more channels finish sooner, and the DMA fan-out does not serialize
  GOLDEN       the exact cycle count of every case, against golden.txt (--update rewrites it)

The exit status is nonzero if any kind fails.
"""

import argparse
import concurrent.futures
import itertools
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
TRACES = HERE / "traces"
GOLDEN = HERE / "golden.txt"
CONFIG = ROOT / "test" / "example.yaml"

FANOUT_CHANNELS = (1, 2, 3, 4, 5, 7, 8, 16, 31, 32, 33, 63, 64, 65, 127, 128)
STRONG_CHANNELS = (1, 2, 4, 8, 16, 32, 64, 128)
SYSTEM_SIZES = (32, 64, 128)  # systems the same small workload runs on
EDGE_CHANNELS = (33, 64, 65, 128)
SPARSE_SYSTEM, SPARSE_CHANNELS = 128, (0, 63, 127)
STRONG_ROWS, WEAK_ROWS, COLUMNS = 256, 4, 64  # as in gen.py

# The commands of the broadcast ISRs in gen.py's fanout traces: every channel issues the same number.
BROADCAST_COMMANDS = ("AF16", "EWMUL16", "MAC", "MAC16", "RDAF16", "RDCP", "RDMAC16", "TMOD", "WRCP", "WRGB", "WRMAC16")

# What the one-channel ISRs of gen.py's fanout traces add to the top channel.
SINGLE_CHANNEL_COMMANDS = {"ACT": 4, "ACT16": 1, "WRA16": 1, "WR": 8, "RD": 8}

# Speedup of strong scaling is at least this fraction of the channel count, while every channel still
# has at least MIN_ROWS rows. With fewer, the fill and read-back that do not shrink with the channel
# count dominate (2 rows per channel at 128 channels measures 71%), and only the monotonic fall is checked.
MIN_EFFICIENCY = 0.8
MIN_ROWS = 4
# Cycles of weak scaling stay within this fraction of the one-channel run. Measured: identical from 1 to
# 128 channels, so this is slack for a timing-model change, not a property of the fan-out.
WEAK_TOLERANCE = 0.05


class Run:
    def __init__(self, trace, channels, overrides=()):
        self.trace, self.channels, self.overrides = trace, channels, tuple(overrides)
        self.key = (trace, channels, self.overrides)
        self.returncode = None
        self.stats = {}
        self.stderr = ""

    def execute(self, binary):
        params = [f"MemorySystem.DRAM.org.channel={self.channels}", *self.overrides]
        command = [str(binary), "-f", str(CONFIG), "-t", str(TRACES / f"{self.trace}.trace")]
        for param in params:
            command += ["-p", param]
        done = subprocess.run(command, capture_output=True, text=True, cwd=ROOT, check=False)
        self.returncode, self.stderr = done.returncode, done.stderr
        for line in done.stdout.splitlines():
            found = re.match(r"\s*(\w+):\s*(-?\d+)\b", line)
            if found:
                self.stats[found[1]] = int(found[2])
        return self

    @property
    def cycles(self):
        return self.stats.get("memory_system_cycles")

    def channel(self, index):
        """One channel's stats, without the `CH<index>_` prefix."""
        prefix = f"CH{index}_"
        return {name[len(prefix) :]: value for name, value in self.stats.items() if name.startswith(prefix)}

    def command(self, index, name):
        return self.stats.get(f"CH{index}_commands_{name}", 0)

    def worked(self):
        """The channels that issued any command."""
        return [
            i for i in range(self.channels) if any(v for k, v in self.channel(i).items() if k.startswith("commands_"))
        ]


class Report:
    def __init__(self):
        self.failures = {"FUNCTIONAL": [], "PERFORMANCE": [], "GOLDEN": []}
        self.checked = {"FUNCTIONAL": 0, "PERFORMANCE": 0, "GOLDEN": 0}

    def check(self, kind, case, ok, why=""):
        self.checked[kind] += 1
        if not ok:
            self.failures[kind].append(f"{case}: {why}")

    def failed(self):
        return any(self.failures.values())


def gemv_channels():
    found = [int(re.search(r"_(\d+)$", p.stem)[1]) for p in TRACES.glob("gemv_4096x4096_*.trace")]
    return sorted(found)


def plan():
    """Every run the checks need, once each."""
    runs = {}

    def add(trace, channels, overrides=()):
        run = Run(trace, channels, overrides)
        return runs.setdefault(run.key, run)

    for c in FANOUT_CHANNELS:
        add(f"fanout_{c}", c)
        add(f"weak_{c}", c)
    for c in (64, 128):
        add(f"fanout_{c}_hex", c)
    for c in STRONG_CHANNELS:
        add(f"strong_{c}", c)
        for size in SYSTEM_SIZES:
            if c <= size:
                add(f"strong_{c}", size)
    add(f"sparse_{SPARSE_SYSTEM}", SPARSE_SYSTEM)
    for c in EDGE_CHANNELS:
        add(f"edge_{c}", c)
    for c in gemv_channels():
        add(f"gemv_4096x4096_{c}", c)
    add("reject_bit64_c64", 64)
    add("reject_bit128_c128", 128)
    add("reject_mem_c64", 64)
    add("reject_zero_mask", 8)
    add("reject_malformed_mask", 8)
    add("fanout_64", 64, ["MemorySystem.DRAM.org.density=131072"])
    return runs


def label(run):
    return f"{run.trace}@{run.channels}" + "".join(f"[{o}]" for o in run.overrides)


def check_functional(runs, report):
    def get(trace, channels, overrides=()):
        return runs[(trace, channels, tuple(overrides))]

    ok = lambda case, cond, why="": report.check("FUNCTIONAL", case, cond, why)

    for c in FANOUT_CHANNELS:
        weak = get(f"weak_{c}", c)
        ok(f"weak_{c}", weak.returncode == 0, f"exit {weak.returncode}")
        run = get(f"fanout_{c}", c)
        case = f"fanout_{c}"
        ok(case, run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()[-200:]}")
        if run.returncode != 0:
            continue
        reference = run.channel(0)
        # The channels below the top one see the same stream, so their whole stats agree.
        stragglers = [i for i in range(c - 1) if run.channel(i) != reference]
        ok(case, not stragglers, f"channels {stragglers[:5]} differ from channel 0")
        for name in BROADCAST_COMMANDS:
            counts = {run.command(i, name) for i in range(c)}
            ok(case, len(counts) == 1 and counts != {0}, f"{name} counts per channel: {sorted(counts)}")
        # The one-channel ISRs ran on the top channel only. What the broadcast ISRs did is channel 0's
        # stream, or for a single channel the same stream from the two-channel run.
        top = c - 1
        reference = reference if c > 1 else get("fanout_2", 2).channel(0)
        for name, extra in SINGLE_CHANNEL_COMMANDS.items():
            got = run.command(top, name) - reference.get(f"commands_{name}", 0)
            ok(case, got == extra, f"channel {top} issued {got} extra {name}, expected {extra}")

    for c in (64, 128):
        decimal, hexadecimal = get(f"fanout_{c}", c), get(f"fanout_{c}_hex", c)
        ok(f"fanout_{c}_hex", decimal.stats == hexadecimal.stats, "hex mask gave different stats than decimal")

    work = STRONG_ROWS * COLUMNS
    for c in STRONG_CHANNELS:
        run = get(f"strong_{c}", c)
        case = f"strong_{c}"
        ok(case, run.returncode == 0, f"exit {run.returncode}")
        counts = [run.command(i, "MAC16") for i in range(c)]
        ok(case, set(counts) == {work // c}, f"MAC16 per channel {sorted(set(counts))}, expected {work // c}")
        ok(case, sum(counts) == work, f"total MAC16 {sum(counts)}, expected {work}")
        for size in SYSTEM_SIZES:
            if c > size:
                continue
            there = get(f"strong_{c}", size)
            same = there.cycles == run.cycles and all(there.channel(i) == run.channel(i) for i in range(c))
            ok(f"strong_{c}@{size}", same, f"{c} channels of a {size}-channel system differ from a {c}-channel system")
            ok(f"strong_{c}@{size}", there.worked() == list(range(c)), f"channels that worked: {there.worked()[:8]}")

    sparse = get(f"sparse_{SPARSE_SYSTEM}", SPARSE_SYSTEM)
    case = f"sparse_{SPARSE_SYSTEM}"
    ok(case, sparse.returncode == 0, f"exit {sparse.returncode}")
    ok(case, sparse.worked() == list(SPARSE_CHANNELS), f"channels that worked: {sparse.worked()}")
    for i in SPARSE_CHANNELS:
        ok(case, sparse.command(i, "MAC16") == WEAK_ROWS * COLUMNS, f"channel {i} MAC16 {sparse.command(i, 'MAC16')}")

    for c in EDGE_CHANNELS:
        run = get(f"edge_{c}", c)
        case = f"edge_{c}"
        ok(case, run.returncode == 0, f"exit {run.returncode}")
        ok(case, run.worked() == [c - 1], f"channels that worked: {run.worked()[:8]}")
        ok(
            case,
            run.command(c - 1, "MAC16") == WEAK_ROWS * COLUMNS,
            f"channel {c - 1} MAC16 {run.command(c - 1, 'MAC16')}",
        )

    for c in gemv_channels():
        run = get(f"gemv_4096x4096_{c}", c)
        case = f"gemv_4096x4096_{c}"
        ok(case, run.returncode == 0, f"exit {run.returncode}")
        counts = [run.command(i, "MAC16") for i in range(c)]
        ok(case, sum(counts) == 65536, f"total MAC16 {sum(counts)}, expected 65536")
        ok(case, max(counts) == min(counts), f"MAC16 per channel from {min(counts)} to {max(counts)}")

    refusals = (
        ("reject_bit64_c64", 64, [], "names channel 64"),
        ("reject_bit128_c128", 128, [], "names channel 128"),
        ("reject_mem_c64", 64, [], "names channel 64"),
        ("reject_zero_mask", 8, [], "selects no channel"),
        ("reject_malformed_mask", 8, [], "malformed channel mask"),
        ("fanout_64", 64, ["MemorySystem.DRAM.org.density=131072"], "does not equal the provided density"),
    )
    for trace, c, overrides, message in refusals:
        run = get(trace, c, overrides)
        case = label(run)
        ok(case, run.returncode != 0, "was accepted")
        ok(case, message in run.stderr, f"message lacks {message!r}: {run.stderr.strip()[-200:]}")


def speedup(table, c):
    """Cycles of the smallest run over this run's, in units of one smallest-run's worth of channels."""
    smallest = min(table)
    return table[smallest] / table[c] * smallest


def cycles_table(runs, trace, counts):
    """Cycles by channel count, for the runs that finished; a run that did not is a FUNCTIONAL failure."""
    table = {}
    for c in counts:
        run = runs[(trace.format(c), c, ())]
        if run.cycles is not None:
            table[c] = run.cycles
    return table


def check_performance(runs, report):
    ok = lambda case, cond, why="": report.check("PERFORMANCE", case, cond, why)
    tables = {}

    strong = cycles_table(runs, "strong_{}", STRONG_CHANNELS)
    gemv = cycles_table(runs, "gemv_4096x4096_{}", gemv_channels())
    weak = cycles_table(runs, "weak_{}", FANOUT_CHANNELS)
    tables["strong"] = strong
    if gemv:
        tables["gemv"] = gemv
    tables["weak"] = weak
    for name, table in (("strong", strong), ("gemv", gemv)):
        if not table:
            continue
        counts = sorted(table)
        for a, b in itertools.pairwise(counts):
            ok(f"{name}_{b}", table[b] < table[a], f"{b} channels took {table[b]} cycles, {a} took {table[a]}")
        for c in counts:
            if name == "strong" and STRONG_ROWS // c < MIN_ROWS:
                continue
            ok(
                f"{name}_{c}",
                speedup(table, c) >= MIN_EFFICIENCY * c,
                f"speedup {speedup(table, c):.1f}x of {c} channels, below {MIN_EFFICIENCY}x",
            )

    for c, cycles in weak.items():  # empty if nothing ran
        drift = cycles / weak[min(weak)] - 1
        ok(f"weak_{c}", abs(drift) <= WEAK_TOLERANCE, f"{cycles} cycles is {drift:+.1%} from the one-channel run")
    return tables


def print_tables(tables):
    weak = tables.pop("weak")
    if weak:
        print("\nweak scaling (fixed work per channel)")
        print(f"{'channels':>9} {'cycles':>9} {'vs 1':>8}")
        for c in sorted(weak):
            print(f"{c:>9} {weak[c]:>9} {weak[c] / weak[min(weak)] - 1:>+8.1%}")
    for name, table in tables.items():
        if not table:
            continue
        counts = sorted(table)
        print(f"\n{name} scaling (fixed total work)")
        print(f"{'channels':>9} {'cycles':>9} {'speedup':>9} {'efficiency':>11}")
        for c in counts:
            speed = speedup(table, c)
            print(f"{c:>9} {table[c]:>9} {speed:>8.1f}x {speed / c:>10.0%}")


def check_golden(runs, report, update):
    cycles = {label(run): run.cycles for run in runs.values() if run.returncode == 0 and run.cycles is not None}
    if update:
        GOLDEN.write_text("".join(f"{name} {value}\n" for name, value in sorted(cycles.items())))
        print(f"wrote {len(cycles)} cases to {GOLDEN.relative_to(ROOT)}")
        return
    golden = dict(line.split() for line in GOLDEN.read_text().splitlines()) if GOLDEN.exists() else {}
    for name, value in sorted(cycles.items()):
        want = golden.get(name)
        report.check("GOLDEN", name, want is not None and int(want) == value, f"timing changed: {want} -> {value}")
    for name in sorted(set(golden) - set(cycles)):
        report.check("GOLDEN", name, False, "in golden.txt but not run")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default=ROOT / "build" / "ramulator2")
    parser.add_argument("--update", action="store_true", help="rewrite golden.txt from this run")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()

    runs = plan()
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        list(pool.map(lambda run: run.execute(args.binary), runs.values()))

    report = Report()
    check_functional(runs, report)
    print_tables(check_performance(runs, report))
    check_golden(runs, report, args.update)

    print()
    for kind, failures in report.failures.items():
        print(f"{kind}: {report.checked[kind] - len(failures)} of {report.checked[kind]} checks passed")
        for failure in failures:
            print(f"  FAIL {failure}")
    return 1 if report.failed() else 0


if __name__ == "__main__":
    sys.exit(main())
