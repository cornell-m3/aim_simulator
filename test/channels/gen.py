#!/usr/bin/env python3
"""Writes the synthetic channel-count traces into traces/. Deterministic; the output is committed.

The GEMV traces (gemv_4096x4096_<C>.trace) are plain data and are not written here.
"""

import pathlib

HERE = pathlib.Path(__file__).resolve().parent
TRACES = HERE / "traces"

FANOUT_CHANNELS = (1, 2, 3, 4, 5, 7, 8, 16, 31, 32, 33, 63, 64, 65, 127, 128)
STRONG_CHANNELS = (1, 2, 4, 8, 16, 32, 64, 128)
STRONG_ROWS = 256  # MAC_ABK rows in the whole workload, split over the channels
WEAK_ROWS = 4  # MAC_ABK rows per channel
COLUMNS = 64  # one DRAM row
REPEAT = 4  # accesses per one-channel ISR
EDGE_CHANNELS = (33, 64, 65, 128)
SPARSE_SYSTEM = 128
SPARSE_CHANNELS = (0, 63, 127)


def mask(channels):
    return sum(1 << c for c in channels)


def every(count):
    return mask(range(count))


def write(name, comment, lines):
    text = "".join(f"# {c}\n" for c in comment) + "".join(f"{line}\n" for line in lines)
    (TRACES / f"{name}.trace").write_text(text)


def broadcast(m):
    """Every broadcast ISR once, so each selected channel sees the same stream."""
    return [
        "W GPR 0",
        "W GPR 1",
        f"AiM WR_GB 1 0 {m}",
        f"AiM WR_BIAS 2 {m}",
        "W CFR 0 1",
        f"AiM MAC_ABK 1 {m} 0",
        f"AiM MAC_SBK 1 {m} 0 0",
        "W CFR 1 0",
        f"AiM EWMUL 1 {m} 0",
        "W CFR 2 1",
        f"AiM AF {m}",
        f"AiM RD_MAC 4 {m}",
        f"AiM RD_AF 6 {m}",
        f"AiM COPY_BKGB 1 {m} 0 0",
        f"AiM COPY_GBBK 1 {m} 0 0",
    ]


def single(channel):
    """The ISRs that address one channel, aimed at `channel`. A lone column access may not issue before
    EOC, so each is repeated; reads follow a SYNC because a read behind a buffered write is forwarded
    from it and never issues."""
    m = 1 << channel
    return (
        [f"AiM WR_ABK 0 {m} 1"]
        + [f"AiM WR_SBK 0 {m} 0 2"] * REPEAT
        + [f"W MEM {channel} 2 3"] * REPEAT
        + ["AiM SYNC"]
        + [f"AiM RD_SBK 8 {m} 1 2"] * REPEAT
        + [f"R MEM {channel} 3 3"] * REPEAT
    )


def fanout(count, render=str):
    top = count - 1
    lines = broadcast(render(every(count))) + ["AiM SYNC"] + single(top) + ["AiM SYNC"]
    return lines + ["AiM EOC"]


def rows(count, m, per_channel):
    """A GB fill, a bias, `per_channel` MAC_ABK rows on every channel in `m`, and the read-back."""
    lines = ["W GPR 0", f"AiM WR_GB {COLUMNS - 1} 0 {m}", f"AiM WR_BIAS 2 {m}", "W CFR 0 1"]
    lines += [f"AiM MAC_ABK {COLUMNS - 1} {m} {row}" for row in range(per_channel)]
    return lines + [f"AiM RD_MAC 4 {m}", "AiM EOC"]


def main():
    TRACES.mkdir(exist_ok=True)

    for count in FANOUT_CHANNELS:
        comment = [f"every broadcast ISR on channels 0..{count - 1}, then the one-channel ISRs on channel {count - 1}"]
        write(f"fanout_{count}", comment, fanout(count))
    for count in (64, 128):
        comment = [f"fanout_{count} with the mask in hex"]
        write(f"fanout_{count}_hex", comment, fanout(count, lambda m: hex(m)))

    for count in STRONG_CHANNELS:
        per_channel = STRONG_ROWS // count
        comment = [f"{STRONG_ROWS} MAC_ABK rows split over {count} channels ({per_channel} each)"]
        write(f"strong_{count}", comment, rows(count, every(count), per_channel))
    for count in FANOUT_CHANNELS:
        comment = [f"{WEAK_ROWS} MAC_ABK rows on each of {count} channels"]
        write(f"weak_{count}", comment, rows(count, every(count), WEAK_ROWS))

    write(
        f"sparse_{SPARSE_SYSTEM}",
        [f"MAC_ABK rows on channels {list(SPARSE_CHANNELS)} of a {SPARSE_SYSTEM}-channel system"],
        rows(SPARSE_SYSTEM, mask(SPARSE_CHANNELS), WEAK_ROWS),
    )
    for count in EDGE_CHANNELS:
        comment = [f"MAC_ABK rows on channel {count - 1}, the highest legal bit of a {count}-channel system"]
        write(f"edge_{count}", comment, rows(count, mask([count - 1]), WEAK_ROWS))

    write("reject_bit64_c64", ["bit 64 names channel 64 on a 64-channel system"], rows(64, 1 << 64, 1))
    write("reject_bit128_c128", ["bit 128 names channel 128 on a 128-channel system"], rows(128, 1 << 128, 1))
    write("reject_mem_c64", ["a MEM access to channel 64 on a 64-channel system"], ["W MEM 64 0 0", "AiM EOC"])
    write("reject_zero_mask", ["a mask that selects no channel"], rows(8, 0, 1))
    write("reject_malformed_mask", ["a mask that is not a number"], rows(8, "1x2", 1))


if __name__ == "__main__":
    main()
