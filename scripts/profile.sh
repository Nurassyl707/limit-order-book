#!/usr/bin/env bash
# Profile the benchmark with Linux perf.
#   scripts/profile.sh [naive|map|array]        (default: array)
# Needs: perf (sudo apt install linux-tools-common linux-tools-generic),
#        and permission to sample: sudo sysctl kernel.perf_event_paranoid=1
set -euo pipefail

IMPL="${1:-array}"
BUILD_DIR="${BUILD_DIR:-build}"
BIN="$BUILD_DIR/bench_book"
[[ -x "$BIN" ]] || { echo "build first: cmake -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j"; exit 1; }

OUT="perf-results"; mkdir -p "$OUT"

echo "== perf stat ($IMPL): IPC, cache and branch behaviour"
perf stat -e cycles,instructions,branches,branch-misses,L1-dcache-loads,L1-dcache-load-misses,LLC-load-misses \
  -o "$OUT/stat.$IMPL.txt" "$BIN" --only "$IMPL" --reps 3
cat "$OUT/stat.$IMPL.txt"

echo "== perf record ($IMPL): where the cycles go"
perf record -F 2000 --call-graph fp -o "$OUT/perf.$IMPL.data" "$BIN" --only "$IMPL" --reps 3 >/dev/null
perf report -i "$OUT/perf.$IMPL.data" --stdio --no-children --percent-limit 1.5 > "$OUT/report.$IMPL.txt"
head -60 "$OUT/report.$IMPL.txt"

echo
echo "Saved to $OUT/. Optional flame graph (https://github.com/brendangregg/FlameGraph):"
echo "  perf script -i $OUT/perf.$IMPL.data | stackcollapse-perf.pl | flamegraph.pl > $OUT/flame.$IMPL.svg"
