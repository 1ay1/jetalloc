#!/usr/bin/env bash
# jetalloc — head-to-head proof harness.
# Runs the SAME benchmark binary (raw malloc/free) under each allocator via
# LD_PRELOAD, so the delta is purely the allocator. Reports median of N runs.
#
# Usage: bench/compare.sh [runs]   (default 5)
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
BIN="$here/build/jet_bench_system"
JET="$here/build/libjetalloc.so"
RUNS="${1:-5}"

[ -x "$BIN" ] || { echo "build first: cmake --build build -j12"; exit 1; }

# allocator -> preload path ("" = system glibc)
declare -A LIBS=(
  [jetalloc]="$JET"
  [tcmalloc]="/usr/lib/libtcmalloc_minimal.so"
  [mimalloc]="/usr/lib/libmimalloc.so"
  [jemalloc]="/usr/lib/libjemalloc.so"
  [glibc]=""
)
ORDER=(jetalloc tcmalloc mimalloc jemalloc glibc)

median(){ printf '%s\n' "$@" | sort -n | awk '{v[NR]=$1}END{print v[int((NR+1)/2)]}'; }

echo "=== jetalloc head-to-head: median of $RUNS runs (Mops/s, higher=better) ==="
printf "%-10s %10s %10s %10s %10s\n" allocator small-fix mixed thread prod/cons
for a in "${ORDER[@]}"; do
  lib="${LIBS[$a]}"
  [ -n "$lib" ] && [ ! -e "$lib" ] && continue   # skip missing competitors
  s=(); m=(); t=(); p=()
  for _ in $(seq "$RUNS"); do
    out=$([ -z "$lib" ] && "$BIN" 2>&1 || LD_PRELOAD="$lib" "$BIN" 2>&1)
    s+=($(echo "$out"|grep small-fixed|awk '{print $3}'))
    m+=($(echo "$out"|grep mixed|awk '{print $3}'))
    t+=($(echo "$out"|grep threaded|awk '{print $3}'))
    p+=($(echo "$out"|grep prod/cons|awk '{print $3}'))
  done
  printf "%-10s %10.0f %10.0f %10.0f %10.0f\n" \
    "$a" "$(median "${s[@]}")" "$(median "${m[@]}")" "$(median "${t[@]}")" "$(median "${p[@]}")"
done
