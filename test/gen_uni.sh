#!/usr/bin/env bash
# Generate test/uni_<N>.trace where N is the number of active channels.
#
# Each round, every channel in 0..N-1 runs WR_BIAS + MAC_ABK + RD_MAC.
# 10 rounds total, then ISR_EOC.
#
# Usage: ./gen_uni.sh <N>           # emits test/uni_<N>.trace
#        ./gen_uni.sh               # regenerates 1, 2, 4, 8, 16, 32

set -euo pipefail

emit() {
    local n=$1
    local out
    out="$(dirname "$0")/uni_${n}.trace"
    {
        echo "# unified-mode equivalent, ${n} channel(s) active, 10 rounds"
        for ((round = 0; round < 10; round++)); do
            for ((ch = 0; ch < n; ch++)); do
                local mask
                printf -v mask "0x%08x" "$((1 << ch))"
                echo "AiM WR_BIAS 4 ${mask}"
                echo "AiM MAC_ABK 63 ${mask} ${round}"
                echo "AiM RD_MAC 4 ${mask}"
            done
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
