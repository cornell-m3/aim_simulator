#!/usr/bin/env bash
# Regression runner.
#
# For each design branch (experiments-design-A, -B, -C, -D), this script
#   1. checks out the branch
#   2. rebuilds the binary
#   3. runs every trace in TRACES against build/ramulator2
#   4. extracts the key stats (memory_system_cycles, total_num_AiM_ISR_*,
#      total_num_wait_read_stalls, a few representative per-channel cycles)
#   5. diffs the extracted stats against test/golden/<branch>/<trace>.txt
#
# Exits 0 if all diffs are empty; nonzero on any mismatch.
#
# Usage:
#   ./test/run_regression.sh                # diff against goldens
#   ./test/run_regression.sh --update       # regenerate goldens
#   ./test/run_regression.sh --only <name>  # restrict to one branch (A/B/C/D)
#
# Leaves the repo on the branch that was checked out at start.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

UPDATE=0
ONLY=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --update) UPDATE=1; shift;;
        --only)   ONLY="$2"; shift 2;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

STARTING_BRANCH="$(git rev-parse --abbrev-ref HEAD)"

# Stash the goldens dir to a tempdir before we start switching branches; the
# goldens are only committed on the cleanup branch and would vanish from disk
# after `git checkout experiments-design-A`. We read from / write to the stash
# throughout, and copy back at exit.
GOLDEN_STASH="$(mktemp -d)"
if [[ -d test/golden ]]; then
    cp -r test/golden/* "$GOLDEN_STASH/" 2>/dev/null || true
fi

cleanup() {
    git checkout "$STARTING_BRANCH" >/dev/null 2>&1 || true
    if [[ "$UPDATE" -eq 1 ]]; then
        mkdir -p test/golden
        cp -r "$GOLDEN_STASH/"* test/golden/ 2>/dev/null || true
    fi
    rm -rf "$GOLDEN_STASH"
}
trap cleanup EXIT

BRANCHES=(A B C D D-cleanup)
TRACES=(uni_1 uni_2 uni_4 uni_8 uni_16 uni_32
        bcast_1 bcast_2 bcast_4 bcast_8 bcast_16 bcast_32
        sync_mid)

# Stat keys to capture. Cheap grep filter; anything matching the regex below
# from the stdout of ramulator2 ends up in the golden.
STAT_REGEX='memory_system_cycles|total_num_wait_read_stalls|total_num_AiM_ISR_(EOC|SYNC|MAC_ABK|RD_MAC|WR_BIAS)_requests|CH(0|15|31)_cycles_AiM_ISR_(MAC_ABK|RD_MAC|WR_BIAS)'

mkdir -p test/golden

fail=0
for letter in "${BRANCHES[@]}"; do
    if [[ -n "$ONLY" && "$letter" != "$ONLY" ]]; then continue; fi
    branch="experiments-design-${letter}"
    echo "=== $branch ==="
    git checkout "$branch" >/dev/null
    (cd build && make -j8 >/dev/null 2>&1)

    mkdir -p "${GOLDEN_STASH}/${branch}"
    for trace in "${TRACES[@]}"; do
        trace_file="test/${trace}.trace"
        if [[ ! -f "$trace_file" ]]; then
            echo "  skip ${trace} (no file)"; continue
        fi
        golden="${GOLDEN_STASH}/${branch}/${trace}.txt"
        actual="$(timeout 120 ./build/ramulator2 -f test/example.yaml -t "$trace_file" 2>&1 \
                  | grep -E "$STAT_REGEX" | sed -E 's/[[:space:]]+#.*$//' | sort)"
        if [[ "$UPDATE" -eq 1 ]]; then
            printf '%s\n' "$actual" > "$golden"
            echo "  updated  ${trace}"
        elif [[ ! -f "$golden" ]]; then
            echo "  MISSING  ${trace}: no golden at $golden"
            fail=1
        else
            if diff -u "$golden" <(printf '%s\n' "$actual") >/dev/null; then
                echo "  ok       ${trace}"
            else
                echo "  MISMATCH ${trace}:"
                diff -u "$golden" <(printf '%s\n' "$actual") | sed 's/^/    /'
                fail=1
            fi
        fi
    done
done

exit "$fail"
