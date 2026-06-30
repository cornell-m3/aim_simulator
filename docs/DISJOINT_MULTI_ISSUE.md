# Disjoint-channel multi-issue dispatch

This page describes the per-tick multi-issue extension to the AiM memory system pipeline, layered on top of design D. Read `AIM_MEMORY_SYSTEM.md` first for the per-channel pipeline and FIFO-join model.

## Motivation

Workloads like SpMV in BSB-ELLPACK form (`benchmarks/spmv_bell16_ch.py`) emit ~99% of their AiM ISRs as `WR_GB` with `mask = 1<<ch_idx` — a single bit per ISR. The pre-existing Phase 1 in `src/memory_system/impl/aim_DRAM_system.cpp` decomposed one host_req per tick:

```cpp
if (!host_request_queue.empty()) {
    decompose(host_request_queue.front());
    host_request_queue.pop();
}
```

With N single-channel WR_GBs total in a trace, total runtime is floored at N cycles regardless of channel count. The per-channel pipelines that Phase 2 drains in parallel never get fed fast enough to do their work in parallel.

A sweep on wiki-RfA confirmed the symptom:

| CH | sim_cycles | speedup over ch=1 | CH0_idle | CH0_idle% |
|---:|-----------:|------------------:|---------:|----------:|
|  1 |    946,432 |             1.00× |    1,392 |     0.1%  |
|  2 |    550,897 |             1.72× |    5,523 |     1.0%  |
|  4 |    314,202 |             3.01× |    7,192 |     2.3%  |
|  8 |    174,321 |             5.43× |    3,416 |     2.0%  |
| 16 |     98,196 |             9.64× |    6,612 |     6.7%  |
| 32 |     95,003 |             9.96× |   44,187 |    46.5%  |

The knee is at ch=16 (sim_cycles ≈ WR_GB count = 91,252). Going to ch=32 buys 3% throughput and 6.7× per-channel idle. The bottleneck is dispatch, not channel-side execution.

## The pipeline analogy: in-order superscalar

The system is essentially a small in-order superscalar pipeline. The mapping:

| AiM DMA | Superscalar CPU |
|---|---|
| Frontend (`aim_trace.cpp`) | Fetch unit |
| `fetch_budget` | Fetch width |
| `host_request_queue` (2M slots) | ROB / instruction queue |
| Phase 1 decompose | Decode + dispatch + issue |
| `decompose_budget` | Issue width |
| Disjoint-mask check | Structural-hazard check (per-channel FU port) |
| `channel_pending_queues[ch]` | Reservation stations |
| Per-channel controllers | Functional units |
| Phase 2 parallel drain | Multiple FUs executing concurrently |
| FIFO stop on conflict | In-order issue |
| (Rejected) skip on conflict | Out-of-order issue (Tomasulo) |
| Full-mask ISRs (SYNC/EOC/MAC_ABK) | Serializing instructions (mfence, cpuid) |
| MAC_ABK broadcast | SIMD/vector op |
| Single-channel WR_GB | Scalar op |
| CFR write read by later AIM op | Architectural state write, no rename |
| EOC | Pipeline drain / fence |

Channels are unusually "fat" FUs — physically independent, no shared register file, no bypass network. That's why the design plausibly scales to 32-wide issue, much wider than any commodity CPU.

## Two knobs, not one

| Knob | Lives on | Models |
|---|---|---|
| `fetch_budget` | AiMTrace frontend | Host bus width (physical I/O) |
| `decompose_budget` | AiMDRAMSystem | DMA internal demux width |

Effective steady-state dispatch rate ≈ `min(fetch_budget, decompose_budget)`. Lifting only one is a no-op — the other becomes the bottleneck. The 2M-deep `host_request_queue` absorbs transients (startup bursts, brief stalls) but cannot manufacture steady supply beyond what the frontend feeds.

Separating them lets future research model asymmetric chips (e.g., narrow host bus + wide internal demux).

## Default = num_channels

The disjoint-mask constraint mathematically caps the per-tick useful budget at `num_channels` — you can't select more pairwise-disjoint masks than there are channels. Higher budgets are wasted.

Both knobs default to a sentinel value `0`, resolved at init/setup time to `num_channels`. Users get the upper-bound config out of the box. Setting a smaller value enables ablation studies (sweep budget ∈ {1,2,4,8,16,32} to see where the knee lands) or models a narrower host port for realism studies.

## FIFO stop on conflict: why not skip?

When the head of `host_request_queue` has a channel mask that conflicts with already-decomposed reqs this tick, we **break** the loop (head waits for next tick). The alternative — skip the head, try the next req — would extract more issue width but breaks two dependencies:

1. **CFR writes mutate `CFR_values` inline** at `aim_DRAM_system.cpp:411`. Subsequent AIM ISRs read those values during their own `decompose()` (e.g., ISR_AF reads AFM at `:344`; ISR_MAC_ABK reads BROADCAST/EWMUL_BG at `:349,:352`). Reordering an AIM op past a CFR write silently changes its operand.
2. **`pending_callbacks` is a FIFO** (`:83, :269, :336`) walked head-first by `receive()` at `:483-500`. Reordering decompositions reorders the callbacks, breaking SYNC/EOC barrier semantics.

Skip-on-conflict would require register-renaming `CFR_values` per issue slot — a much larger change for marginal extra ILP on realistic workloads.

## Cost of full-mask blocking

A broadcast ISR (SYNC, EOC, or any AIM op with `mask = all_channels_mask()`) sets `mask_taken = all-1s` after its iteration. The next iteration's disjoint check necessarily fails → loop breaks. The broadcast consumes 1 budget slot; the remaining slots that tick are unused.

This is correct, not a defect. SYNC must complete before later ops anyway; broadcasts already fan out to every channel via the per-channel queues, so Phase 2 saturates the back-end. The "wasted" issue slots correspond to *cycles where no further dispatch is possible*, not lost throughput.

Quantitative cost on the wiki-RfA trace:

- 172 MAC_ABK + 23 RD_MAC + 23 WR_BIAS + 1 EOC ≈ **219 broadcast-shape ISRs** out of ~92,000 total = 0.24%.
- At budget=32, wasted slot-opportunities ≈ 31 × 219 ≈ 6,800 out of ~92,000 × 32 ≈ 2.9M total slot-budget ≈ **sub-0.3% efficiency loss**.

In a workload dominated by broadcasts (e.g., dense GEMM with mostly MAC_ABK), the budget would barely matter — but in that regime per-channel parallel drain already saturates the back-end, and dispatch isn't the bottleneck.

## Expected results

Re-run the sweep `ch ∈ {1,2,4,8,16,32}` × `budget ∈ {1,2,4,8,16,32}`:

- At `budget=B`, the dispatch ceiling ≈ `WR_GB_count / B` cycles for any WR_GB-dominated trace.
- The channel-side floor ≈ `WR_GB_count / ch × 9` cycles (9 = nCWLGB + nBL from GDDR6_AiM_timing).
- Total sim_cycles ≈ `max(dispatch_ceiling, channel_floor) + small constant overhead`.
- The "knee" channel count where doubling `ch` stops helping slides right with `B`. At `budget=1` the knee is at `ch=16`; at `budget=32` the knee should approach `ch=32` with sim_cycles ≈ 30k on wiki-RfA (≈ 3× faster than budget=1, ch=32).

## Configuration

In `aim_config.yaml`:

```yaml
Frontend:
  impl: AiMTrace
  clock_ratio: 1
  # fetch_budget: 0          # 0 = auto = num_channels.
                              # Set lower to model a narrower host port.

MemorySystem:
  impl: AiMDRAM
  clock_ratio: 1
  # decompose_budget: 0      # 0 = auto = num_channels.
                              # Set lower for ablation studies.
```

The knobs should generally be set to the same value. Setting `fetch_budget < decompose_budget` starves the dispatcher; setting `fetch_budget > decompose_budget` grows the host_request_queue without benefit.
