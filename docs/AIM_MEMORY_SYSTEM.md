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
