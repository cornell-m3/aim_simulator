#!/usr/bin/env bash
# Print a Markdown table of memory_system_cycles across the uni_N scaling
# family for each design branch plus the pre-decouple baseline.
#
# Row labels are branches; columns are N = 1, 2, 4, 8, 16, 32. Each cell is
# the simulator's `memory_system_cycles` stat for test/uni_<N>.trace.
#
# Expected shape:
#   - 0a413a6 (pre-decouple, global stall gate): linear in N
#   - experiments-design-{A,B,C,D,D-cleanup}: roughly constant in N
#
# The contrast is the headline result of the decouple-stalls effort.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

STARTING_BRANCH="$(git rev-parse --abbrev-ref HEAD)"

# 0a413a6 (the baseline) predates the uni_N / bcast_N traces, so we stash them
# into a scratch dir and feed the binary their absolute paths. That keeps all
# branches on the same trace corpus without committing onto historical refs.
TRACE_SCRATCH="$(mktemp -d)"
for n in 1 2 4 8 16 32; do
    cp "test/uni_${n}.trace" "$TRACE_SCRATCH/"
    cp "test/bcast_${n}.trace" "$TRACE_SCRATCH/"
done

cleanup() {
    git checkout "$STARTING_BRANCH" >/dev/null 2>&1 || true
    rm -rf "$TRACE_SCRATCH"
}
trap cleanup EXIT

# ref → display name
declare -a REFS=(
    "0a413a6:0a413a6 (pre-decouple)"
    "experiments-design-A:design A"
    "experiments-design-B:design B"
    "experiments-design-C:design C"
    "experiments-design-D:design D"
    "experiments-design-D-cleanup:design D + cleanup"
)
NS=(1 2 4 8 16 32)

run_one() {
    local trace=$1
    timeout 120 ./build/ramulator2 -f test/example.yaml -t "$trace" 2>&1 \
        | grep memory_system_cycles | awk '{print $2}'
}

print_table() {
    local family=$1   # "uni" or "bcast"
    echo
    echo "### ${family}_N (channel_mask = bits 0..N-1 in $( [[ $family = uni ]] && echo 'one-channel-per-host_req' || echo 'one-host_req-per-N-channels' ))"
    echo
    # Header
    printf "| Branch                   |"
    for n in "${NS[@]}"; do printf " N=%-4d|" "$n"; done
    printf "\n"
    printf '|--------------------------|'
    for _ in "${NS[@]}"; do printf '%s' "-------|"; done
    printf "\n"

    for entry in "${REFS[@]}"; do
        ref="${entry%%:*}"
        name="${entry#*:}"
        git checkout "$ref" >/dev/null 2>&1
        (cd build && make -j8 >/dev/null 2>&1)
        printf "| %-24s |" "$name"
        for n in "${NS[@]}"; do
            trace="${TRACE_SCRATCH}/${family}_${n}.trace"
            cycles=$(run_one "$trace")
            printf " %-6s|" "$cycles"
        done
        printf "\n"
    done
}

print_table uni
print_table bcast
