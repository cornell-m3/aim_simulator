#!/usr/bin/env python3
"""Checks what many-channels adds: a channel mask is a set of indices of any width, and the channel
count is whatever `MemorySystem.DRAM.org.channel` says.

    python3 test/channels/run.py [--binary build/ramulator2]

Each case is one line of trace on test/example.yaml. What an ISR does on one channel is the other
traces' business; here only which channels it reaches, and that a channel the system lacks is refused.
"""

import argparse
import dataclasses
import functools
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
CONFIG = ROOT / "test" / "example.yaml"

AF = "AiM AF {mask}"
MAC_ABK = "AiM MAC_ABK 0 {mask} 0"
MEM_WRITE = "W MEM {channel} 0 0"

# Both sides of the old 32-channel cap and of a 64-bit word of the mask, and a count that is no power of two.
SIZES = (1, 3, 32, 33, 64, 65, 128)
# Systems the same workload must cost the same on.
SYSTEM_SIZES = (32, 64, 128)
WEAK_ROWS_PER_CHANNEL = 4
WEAK_TOLERANCE = 0.05


@dataclasses.dataclass
class Run:
    returncode: int
    stats: dict[str, int]
    stderr: str


def build_mask(channels):
    return sum(1 << channel for channel in channels)


def run_ramulator(lines, n_channels, binary, overrides=()):
    """`lines` and an EOC on a system of `n_channels`."""
    command = [str(binary), "-f", str(CONFIG), "-p", f"MemorySystem.DRAM.org.channel={n_channels}"]
    for override in overrides:
        command += ["-p", override]
    with tempfile.NamedTemporaryFile("w", suffix=".trace") as trace:
        trace.write("".join(f"{line}\n" for line in [*lines, "AiM EOC"]))
        trace.flush()
        done = subprocess.run([*command, "-t", trace.name], capture_output=True, text=True, cwd=ROOT, check=False)
    stats = {name: int(value) for name, value in re.findall(r"^\s*(\w+):\s*(-?\d+)\b", done.stdout, re.MULTILINE)}
    return Run(done.returncode, stats, done.stderr)


def list_working_channels(run):
    """The channels that issued any command."""
    working = set()
    for name, value in run.stats.items():
        found = re.match(r"CH(\d+)_commands_", name)
        if found and value:
            working.add(int(found[1]))
    return working


def list_channel_stats(run, channels):
    """The cycle count and the counters of `channels`."""
    stats = {"memory_system_cycles": run.stats["memory_system_cycles"]}
    for name, value in run.stats.items():
        found = re.match(r"CH(\d+)_", name)
        if found and int(found[1]) in channels:
            stats[name] = value
    return stats


def check_ran(run, trace):
    if run.returncode != 0:
        raise AssertionError(f"{trace} failed with exit {run.returncode}: {run.stderr.strip()[-200:]}")


def check_reaches(template, n_channels, channels, binary):
    """`template` (a line with `{mask}` or `{channel}`) on `channels` of `n_channels` works those and no others."""
    line = template.format(mask=build_mask(channels), channel=min(channels))
    run = run_ramulator([line], n_channels, binary)
    check_ran(run, line)
    working = list_working_channels(run)
    if working != set(channels):
        raise AssertionError(
            f"{line} on {n_channels} channels\n    did not work: {sorted(set(channels) - working)}"
            f"\n    worked unasked: {sorted(working - set(channels))}"
        )


def check_hex_matches_decimal(template, n_channels, binary):
    mask = build_mask(range(n_channels))
    decimal = run_ramulator([template.format(mask=mask)], n_channels, binary)
    hexadecimal = run_ramulator([template.format(mask=hex(mask))], n_channels, binary)
    check_ran(decimal, template)
    check_ran(hexadecimal, template)
    if decimal.stats != hexadecimal.stats:
        raise AssertionError(f"{template} on {n_channels} channels: mask {hex(mask)} reads differently from {mask}")


def check_system_size_is_free(template, channels, binary):
    """`channels` cost the same and issue the same on every system in SYSTEM_SIZES."""
    line = template.format(mask=build_mask(channels), channel=min(channels))
    seen = []
    for n_channels in SYSTEM_SIZES:
        run = run_ramulator([line], n_channels, binary)
        check_ran(run, line)
        seen.append(list_channel_stats(run, channels))
    if any(stats != seen[0] for stats in seen):
        raise AssertionError(f"{line} differs between systems of {SYSTEM_SIZES} channels")


def check_refused(lines, n_channels, message, binary, overrides=()):
    run = run_ramulator(lines, n_channels, binary, overrides)
    if run.returncode == 0:
        raise AssertionError(f"{lines} on {n_channels} channels was accepted")
    if message not in run.stderr:
        raise AssertionError(f"{lines} on {n_channels} channels: expected {message!r}, got {run.stderr.strip()[-200:]}")


def check_weak_scaling_is_flat(sizes, binary):
    """The same rows on every channel take the same cycles however many channels there are."""
    cycles = {}
    for n_channels in sizes:
        mask = build_mask(range(n_channels))
        rows = [f"AiM MAC_ABK 63 {mask} {row}" for row in range(WEAK_ROWS_PER_CHANNEL)]
        run = run_ramulator(rows, n_channels, binary)
        check_ran(run, rows[0])
        cycles[n_channels] = run.stats["memory_system_cycles"]
    smallest = cycles[min(cycles)]
    if any(abs(value / smallest - 1) > WEAK_TOLERANCE for value in cycles.values()):
        raise AssertionError(f"cycles by channel count: {cycles}")


def enumerate_cases(binary):
    """`(kind, name, check)` for every case, in the order the properties are listed."""
    reaches = functools.partial(check_reaches, binary=binary)
    refused = functools.partial(check_refused, binary=binary)

    for n_channels in SIZES:
        yield (
            "FUNCTIONAL",
            f"AF on all {n_channels} channels",
            functools.partial(reaches, AF, n_channels, range(n_channels)),
        )
    yield "FUNCTIONAL", "AF on {0, 63, 127} of 128", functools.partial(reaches, AF, 128, {0, 63, 127})
    for n_channels in (33, 64, 65, 128):
        yield (
            "FUNCTIONAL",
            f"AF on channel {n_channels - 1} of {n_channels}",
            functools.partial(reaches, AF, n_channels, {n_channels - 1}),
        )
    yield "FUNCTIONAL", "MEM write to channel 64 of 65", functools.partial(reaches, MEM_WRITE, 65, {64})
    yield (
        "FUNCTIONAL",
        "128-channel mask in decimal and hex",
        functools.partial(check_hex_matches_decimal, AF, 128, binary),
    )
    yield (
        "FUNCTIONAL",
        "MAC_ABK on 0..3 of 32, 64 and 128",
        functools.partial(check_system_size_is_free, MAC_ABK, range(4), binary),
    )

    for n_channels in (64, 128):
        yield (
            "FUNCTIONAL",
            f"refuse mask bit {n_channels} of {n_channels}",
            functools.partial(
                refused, [AF.format(mask=build_mask([n_channels]))], n_channels, f"names channel {n_channels}"
            ),
        )
    yield (
        "FUNCTIONAL",
        "refuse MEM to channel 64 of 64",
        functools.partial(refused, [MEM_WRITE.format(channel=64)], 64, "names channel 64"),
    )
    yield "FUNCTIONAL", "refuse mask 0", functools.partial(refused, [AF.format(mask=0)], 8, "selects no channel")
    yield (
        "FUNCTIONAL",
        "refuse mask 1x2",
        functools.partial(refused, [AF.format(mask="1x2")], 8, "malformed channel mask"),
    )
    yield (
        "FUNCTIONAL",
        "refuse a density that does not fit 64 channels",
        functools.partial(
            refused,
            [AF.format(mask=1)],
            64,
            "does not equal the provided density",
            overrides=["MemorySystem.DRAM.org.density=131072"],
        ),
    )

    yield "PERFORMANCE", "weak scaling is flat", functools.partial(check_weak_scaling_is_flat, (1, 8, 65, 128), binary)


def report_failures(failed, n_cases):
    for kind, n_kind in n_cases.items():
        print(f"{kind}: {n_kind - len(failed[kind])} of {n_kind} passed")
        for name, why in failed[kind]:
            print(f"  FAIL {name}\n    {why}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default=ROOT / "build" / "ramulator2")
    args = parser.parse_args()

    n_cases = {"FUNCTIONAL": 0, "PERFORMANCE": 0}
    failed = {"FUNCTIONAL": [], "PERFORMANCE": []}
    for kind, name, check in enumerate_cases(args.binary):
        n_cases[kind] += 1
        try:
            check()
        except AssertionError as error:
            failed[kind].append((name, error))
    report_failures(failed, n_cases)
    return 1 if any(failed.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
