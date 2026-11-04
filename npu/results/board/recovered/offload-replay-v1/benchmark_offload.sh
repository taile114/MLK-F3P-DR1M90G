#!/bin/sh
set -eu
root=/mnt/mmcblk0p2/npu-tests/offload-v2/f3p-offload-v2
cd "$root"
trap 'echo $? > benchmark.exit; sync' 0
export F3P_VERIFY_INT=0
trial() {
 echo "trial=$1 case=$2"
 case "$2" in
  neon) sh run.sh neon 0;;
  head2) sh run.sh hybrid 0 qparams_head.tsv;;
  ff4) sh run.sh hybrid 0 qparams_ff4.tsv;;
  ff6) sh run.sh hybrid 0 qparams_ff6.tsv;;
  input_ff5) sh run.sh hybrid 0 qparams_input_ff5.tsv conv_input.txt;;
 esac
}
for name in neon head2 ff4 ff6 input_ff5; do trial 1 "$name"; done
for name in input_ff5 ff6 ff4 head2 neon; do trial 2 "$name"; done
for name in ff4 input_ff5 neon ff6 head2; do trial 3 "$name"; done
echo result=paired_benchmark_completed
