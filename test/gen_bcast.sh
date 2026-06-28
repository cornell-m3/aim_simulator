#!/usr/bin/env bash
# Generate test/bcast_<N>.trace where N is the number of channels broadcast to.
#
# Each round emits exactly three host_reqs whose channel_mask selects the first
# N channels at once (bits 0..N-1). The simulator's decompose() fans each host_req
# out to N aim_reqs (or N * (opsize+1) for the burst op), all in a single tick.
#
# Same per-channel work as uni_<N>.trace, but ~N× fewer host_reqs — useful for
# isolating dispatch-rate effects from per-channel pipeline behavior.
#
# Usage: ./gen_bcast.sh <N>           # emits test/bcast_<N>.trace
#        ./gen_bcast.sh               # regenerates 1, 2, 4, 8, 16, 32

set -euo pipefail

emit() {
    local n=$1
    local out
    out="$(dirname "$0")/bcast_${n}.trace"
    local mask
    if (( n >= 32 )); then
        printf -v mask "0x%08x" $((0xffffffff))
    else
        printf -v mask "0x%08x" $(((1 << n) - 1))
    fi
    {
        echo "# broadcast-mode, ${n} channel(s) active per host_req, 10 rounds"
        for ((round = 0; round < 10; round++)); do
            echo "AiM WR_BIAS 4 ${mask}"
            echo "AiM MAC_ABK 63 ${mask} ${round}"
            echo "AiM RD_MAC 4 ${mask}"
        done
        echo "AiM ISR_EOC"
    } > "$out"
    echo "wrote $out ($(wc -l < "$out") lines)"
}

if [[ $# -eq 1 ]]; then
    emit "$1"
else
    for n in 1 2 4 8 16 32; do
        emit "$n"
    done
fi
