# AiM memory system: per-channel pipeline + FIFO join

This page describes the AiM memory system (`src/memory_system/impl/aim_DRAM_system.cpp`) and the four experimental designs (A/B/C/D) that decouple per-channel stalls. It assumes you have read the top-level `README.md` for the AiM ISR model.

## The pipeline

The memory system sits between the AiM trace frontend and the 32 GDDR6 channel controllers. Each tick, it does three things:

```
host_request_queue
        │
        ▼
   ┌─────────┐         ┌──────────────────┐
   │ decompose│ ─────▶ │ channel_pending_  │
   │ (per     │        │ queues[0..31]     │
   │ host_req)│        └──────────────────┘
   └─────────┘                 │
                               ▼  (drain phase, one send per channel per tick)
                       ┌──────────────────┐
                       │ IDRAMController  │
                       │   [0..31]        │
                       └──────────────────┘
                               │
                               ▼  (controller fires aim_req.callback on completion)
                       receive(req): stalls[ch]--
```

`decompose()` splits one host_req (e.g. `ISR_MAC_ABK 63 0x00000001 0`) into the per-channel aim_reqs it implies, pushing them onto `channel_pending_queues[ch]`. The drain phase sends one aim_req per channel per tick (subject to the controller's queue accepting it). Channels never share state at the send level, so a channel parked on a blocking `ISR_RD_MAC` doesn't gate another channel's pipeline.

## Decoupling stalls

Two state variables shape the dispatch:

- `stalls[ch]` — count of in-flight blocking aim_reqs on channel ch. Drain skips any channel with `stalls[ch] > 0`. Decremented in `receive()` when the controller signals completion.
- `last_completed_host_id[ch]` — high-water mark, the largest host_req_id whose blocking aim_req has completed on channel ch. Advances only in `receive()`, so it cannot race ahead of real completions.

The pre-decouple `fixes` commit had a single global stall counter that gated the *entire* dispatcher. Any in-flight blocking op on ch0 stopped dispatch to ch5. With the per-channel design, channels run independently. See the scaling sweep in `test/scaling_table.sh`:

| Branch              | N=1   | N=2   | N=4   | N=8   | N=16  | N=32  |
|---------------------|-------|-------|-------|-------|-------|-------|
| 0a413a6 (baseline)  | 2860  | 5682  | 11362 | 22722 | 45442 | 90882 |
| design A / B / C / D| 2860  | 2863  | 2869  | 2881  | 2905  | 2953  |

Baseline scales linearly in the channel count; the decoupled designs stay flat. (Workload: 10 rounds of `WR_BIAS + MAC_ABK 63 + RD_MAC` per active channel + `ISR_EOC`.)

There's a companion `bcast_N` family that packs the same per-channel work into a single host_req per round (channel_mask = bits 0..N-1, fanning out to N channels in one decompose). The numbers are flat at 2860 cycles across *every* branch — including the pre-decouple baseline:

| Branch              | N=1   | N=2   | N=4   | N=8   | N=16  | N=32  |
|---------------------|-------|-------|-------|-------|-------|-------|
| 0a413a6 (baseline)  | 2860  | 2860  | 2860  | 2860  | 2860  | 2860  |
| design A / B / C / D| 2860  | 2860  | 2860  | 2860  | 2860  | 2860  |

This is the contrapositive of the headline result: the pre-decouple bottleneck was the global stall gate *between host_reqs*. Pack the same work into 31 host_reqs (bcast) instead of 30·N+1 (uni) and the bottleneck doesn't fire. The decouple-stalls effort closed a gap that only exists when traces emit many small host_reqs — exactly the access pattern of real workloads.

## The host-callback join

The frontend attaches a callback to `ISR_EOC` (and would attach one to any other host_req it wants notification on). That callback fires when the memory system has finished the host_req's work. Designs A and B implement this as a global "all channels free" barrier; designs C and D implement it as a per-host barrier on a required-channels mask.

| Design | Send path                       | Join model                                          |
|--------|---------------------------------|-----------------------------------------------------|
| A      | dispatch + retry (two sites)    | all-free: stalls=0 AND queue empty everywhere       |
| B      | drain only (one site)           | all-free: stalls=0 AND queue empty everywhere       |
| C      | dispatch + retry (two sites)    | required_channels FIFO + last_completed_host_id     |
| D      | drain only (one site)           | required_channels FIFO + last_completed_host_id     |

Design D is the cleanest of the four: a single send path eliminates the class of "did I bump stalls in both paths?" bugs by construction, and the high-water-mark join is race-free without any "empty queue" clause (the FIFO can't fire on aim_reqs that haven't actually completed). The cleanup branch (`experiments-design-D-cleanup`) builds on D.

## What gates dispatching the next host_req

After D's `was_AiM_request_remaining` gate is removed, `decompose()` runs once per tick unconditionally. The dispatcher itself never waits — channels are throttled solely by `stalls[ch]` and by the controller's send queue. A new host_req can land in `channel_pending_queues[ch]` even while ch's previous blocking aim_req is in flight; it'll send the moment that channel's stall clears.

## Race-free join (design C / D)

`last_completed_host_id[ch]` only advances when an aim_req actually completes (it's set inside `receive()`, no earlier). The PendingCallback at the head of the FIFO fires when every channel in its `required_channels` mask has caught up to its `host_req_id`. So:

- A freshly-decomposed but unsent aim_req can never trip the barrier (it can't have advanced `last_completed_host_id`).
- An RD_MAC completion on ch5 advances last_completed only as far as RD_MAC's host_req_id, not the EOC's.
- The first time `last_completed_host_id[ch] >= EOC.host_req_id` for every channel in EOC's mask, EOC's callback fires, and the simulator terminates.

## Tools

- `test/gen_uni.sh N` — emit `test/uni_<N>.trace` (or all of 1/2/4/8/16/32 with no args).
- `test/run_regression.sh [--update] [--only X]` — for each design branch, run every trace and diff key stats against the golden in `test/golden/<branch>/<trace>.txt`.
- `test/scaling_table.sh` — print the Markdown table above by sweeping uni_N across each branch and the `0a413a6` baseline.
- `cd build && ctest --output-on-failure` — run the channel-mask helper unit tests (`ramulator-aim-tests`).

## Files

- `src/memory_system/impl/aim_DRAM_system.cpp` — the AiMDRAMSystem class.
- `src/memory_system/impl/aim_channel_mask.h` — `count` and `pop_lowest` helpers (pure-function, unit-tested).
- `src/memory_system/impl/aim_DRAM_system_test.cpp` — gtest cases for the helpers.
- `src/base/request.h` — Request, AiMISR, opcode tables.
- `src/frontend/impl/memory_trace/aim_trace.cpp` — frontend that parses traces and pushes host_reqs into the memory system.

## How this maps to AiM hardware

The simulator's `AiMDRAMSystem` class plays the role of the **AiM DMA + multicasting interconnect** in the SK hynix platform (HW whitepaper Figs 3 and 7); the per-channel `IDRAMController` is the **AiM controller** in HW whitepaper Fig 5. The class name is misleading — it isn't the DRAM itself.

| code | hardware |
|---|---|
| `host_request_queue` | ISR register (MMIO range receiving 256-bit ISR instructions from the host over PCIe) |
| `decompose()` | AiM DMA's "decode ISR → emit sequence of fine-grained AiM requests" stage |
| inner channel loop in decompose | multicasting interconnect duplicating one AiM request across every channel in `CH_MASK` |
| `channel_pending_queues[ch]` | per-channel request stream entering each AiM controller |
| `IDRAMController` | AiM controller (Row Arbiter + Bank/AiM/Refresh engines + Engine Arbiter + PHY) |
| CFR map (`BROADCAST` / `EWMUL_BG` / `AFM`) | controller mode-config bits set by `ISR_WR_CFR` |

`ISR_SYNC` and `ISR_EOC` are not real ISR opcodes — the real set is in SW whitepaper Fig 2 / HW whitepaper Fig 4. They're simulator conventions: `ISR_EOC` is the trace terminator the AiMTrace frontend uses to stop; `ISR_SYNC` is a barrier primitive. Host callbacks are also simulator-only — the real DMA tracks completion via internal sequence numbers.

### The three rate-limit layers

| Layer | Per-tick behavior in the simulator | Hardware analog |
|---|---|---|
| 1. Decompose | 1 host_req per tick; the host_req's *entire* aim_req expansion lands in the channel queues that tick | AiM DMA emitting aim_reqs sequentially over many cycles + interconnect fan-out latency |
| 2. Drain (`controller->send()`) | Per channel, send as many as `stalls==0` allows AND the controller's intake queue accepts (size 64) | multicasting interconnect → controller request port |
| 3. Controller tick | One DRAM command per tick per channel, subject to GDDR6-AiM timing (`tCK`, `tCCDS`, ACT→RD/WR, refresh, …) | AiM controller scheduler + PHY |

**Layer 3 dominates wall time for compute-bound workloads.** Ramulator's DRAM timing model serializes commands per channel at the real per-command pace. So even when `MAC_ABK 63` lands 64 aim_reqs into a channel's controller queue in a single decompose tick, they still drain through the PHY at the real per-command rate.

What the simulator approximates is **when** aim_reqs become available to each controller, not **how fast** the controller processes them:

- **Faithful (Layer 3)**: per-command DRAM timing; per-channel scheduling independence (matches AiM controller Fig 5); bank/bankgroup mapping (4 BG × 4 banks per channel matches HW whitepaper Fig 2); OPSIZE → `opsize + 1` AiM requests; CH_MASK fan-out.
- **Approximate (Layers 1–2)**: DMA emission rate is treated as one ISR per tick (all aim_reqs in that ISR materialize immediately in the channel queues); interconnect transit time and intake-queue contention are not modeled.
- **Invented**: `ISR_SYNC`, `ISR_EOC`, host callbacks.

The Layer-1/2 approximations don't show up in cycle counts for `uni_N` or `bcast_N` because both workloads are compute-bound. They would matter for a workload that stressed the interconnect (high ISR-rate, narrow per-ISR work).

### Implications for the decouple-stalls experiments

1. **The pre-decouple `fixes` global stall gate was a fidelity bug, not just a perf gap.** The HW whitepaper's per-channel AiM controller (Fig 5) has no architectural mechanism for one channel's blocking `RD_MAC` to gate dispatch to another channel — the multicasting interconnect is one-to-many, and each controller has its own scheduler. The pre-decouple code modeled a serialization the hardware doesn't have. The decouple-stalls effort was correcting an artifact, not just optimizing.
2. **`bcast_N` is closer to real workloads than `uni_N`.** The HW whitepaper's GEMV example uses `CH_MASK = 0xF` to hit all 4 channels with a single ISR_MAC_ABK. Production workloads issue ISRs with wide channel masks; `uni_N`'s one-channel-per-ISR pattern is a micro-benchmark that maximally exercises the pre-decouple bottleneck. The 31× headline speedup is what the bottleneck looked like in the worst case; realistic workloads sit closer to the bcast curve.
3. **The flat-vs-linear contrast in `uni_N` is real, but the absolute speedup is workload-dependent.** Widening CH_MASK to span more channels per ISR (broadcast direction) narrows the gap; emitting many one-channel ISRs widens it.

### Source documents

- `docs/aim-spec.pdf` — ISSCC 2022 paper. AiM device architecture (16 banks × 16 PUs, 2KB GB, BWMS multiplier).
- `docs/aim-hardware-whitepaper.pdf` — AiM platform hardware: DMA + multicasting interconnect + per-channel controller; ISR encoding; GEMV example flow.
- `docs/aim-software-whitepaper.pdf` — software stack: PyTorch/ONNX integration, runtime ISR generation, FPGA memory zones.
- `docs/gddr6-spec.pdf` — base GDDR6 standard (not summarized here).
