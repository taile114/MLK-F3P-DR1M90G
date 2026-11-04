#!/bin/sh
set -eu
root=/mnt/mmcblk0p2/npu-tests/offload-v2/f3p-offload-v2
cd "$root"
capture="$root/results_profile_19700101_000417_4190"
for label in linear_x.spline_weight linear_x.base_weight tvgtformer_blocks.0.feed_forward_temp.0.spline_weight svgtformer_blocks.0.feed_forward_spat.0.spline_weight tvgtformer_blocks.1.feed_forward_temp.0.spline_weight svgtformer_blocks.1.feed_forward_spat.0.spline_weight tvgtformer_blocks.0.feed_forward_temp.2.spline_weight svgtformer_blocks.0.feed_forward_spat.2.spline_weight tvgtformer_blocks.1.feed_forward_temp.2.spline_weight svgtformer_blocks.1.feed_forward_spat.2.spline_weight; do
 sh run.sh branch "$label" fc "$capture"
done
for label in linear_x.spline_weight linear_x.base_weight; do
 sh run.sh branch "$label" conv "$capture"
done
