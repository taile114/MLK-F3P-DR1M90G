#!/bin/sh
set -eu
root=/mnt/mmcblk0p2/npu-tests/fused-v3
cd "$root"
test ! -e benchmark.tsv
test ! -e benchmark.exit
trap 'status=$?; echo "$status" > benchmark.exit; sync' 0
printf 'round\tmode\tdirectory\n' > benchmark.tsv
for round in 1 2 3; do
 case "$round" in
  1) order='reference neon cpu_quantized hybrid';;
  2) order='hybrid cpu_quantized neon reference';;
  3) order='neon reference hybrid cpu_quantized';;
 esac
 for mode in $order; do
  log="benchmark-round-$round-$mode.log"
  F3P_VERIFY_INT=0 sh run.sh "$mode" 0 > "$log" 2>&1
  directory=$(sed -n 's/^exit_code=0 results=//p' "$log" | tail -n 1)
  test -n "$directory"
  printf '%s\t%s\t%s\n' "$round" "$mode" "$directory" >> benchmark.tsv
 done
done
echo result=benchmark_completed
