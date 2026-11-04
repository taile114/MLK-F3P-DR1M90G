#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$here"
sha256sum -c SHA256SUMS
# An isolated prefix is used by offline installer tests; normal board updates leave it unset.
prefix=${MLK_UPDATE_ROOT:-}
target="$prefix/opt/qt_play/mlk_ps_local"
link="$prefix/opt/mlk_fault"
service="$prefix/etc/init.d/S99qtapp"
test -L "$link"
old_link=$(readlink "$link")
case "$old_link" in /*) old_runtime="$prefix$old_link" ;; *) old_runtime="${link%/*}/$old_link" ;; esac
old_runtime=$(CDPATH= cd -- "$old_runtime" && pwd)
case "$old_runtime" in "$prefix/opt/mlk_fault-packages/"*) ;; *) echo 'Unexpected runtime location' >&2; exit 1 ;; esac
(cd "$old_runtime" && sha256sum -c "$here/runtime/UNCHANGED_ASSETS.sha256")
version=$(cat runtime/version)
case "$version" in ''|*[!a-zA-Z0-9._-]*) echo 'Invalid runtime version' >&2; exit 1 ;; esac
case "$version" in f3p-fault-single-v1-*) ;; *) exit 1 ;; esac
new_runtime="$prefix/opt/mlk_fault-packages/$version"
new=$(sha256sum mlk_ps_local); new=${new%% *}
actual=$(sha256sum "$target"); actual=${actual%% *}
if [ "$actual" = "$new" ] && [ "$old_runtime" = "$new_runtime" ]; then
    (cd "$new_runtime" && sha256sum -c "$here/runtime/FULL_RUNTIME.sha256")
    echo 'Application and temperature guard library are already installed'
    exit 0
fi
case "$actual" in
  0df57fa0d4aa4002deca8f15f1293ad0d81b6d0d2d826436e616411c051ca400|\
  023bd835f4f32ced8eaeea7f835cee40a545d256df0b3490298d3ac5ccce70de|\
  71629c087b1c80ae72d4ec933680d5a7e338a1533f7b14d100e1810394cd3d79|\
  409fbd7cebabff4f58c63faba1a1ed675750e86efdc9f17cfe1643059072d1b2|\
  4b12958be0019d235bef211c377f9188d985e88ed6a5f2131bf2c932ce724813|\
  9b7039ab2214b52d9c23948a53a7eaad137cb4028efa07416bb7c5c87a4d084e|\
  a7f3465ea429b0e460e853e87619b0e48b7203a520dc8164abaa8797c4f541f5|"$new") ;;
  *) echo 'Existing application differs; review before replacing' >&2; exit 1 ;;
esac
if [ -e "$new_runtime" ]; then
    test ! -L "$new_runtime"
    (cd "$new_runtime" && sha256sum -c "$here/runtime/FULL_RUNTIME.sha256")
else
    stage="$new_runtime.stage-$$"
    test ! -e "$stage"
    mkdir "$stage"
    chmod 755 "$stage"
    for asset in weights.bin weights.tsv qparams.tsv thresholds.bin preprocess.json; do
        cp "$old_runtime/$asset" "$stage/$asset"
    done
    cp runtime/model.json runtime/libmlk_fault.so "$stage/"
    chmod 644 "$stage/"*
    (cd "$stage" && sha256sum -c "$here/runtime/FULL_RUNTIME.sha256")
    mv "$stage" "$new_runtime"
fi
short=$(printf '%.16s' "$actual")
backup="$target.before-thermal-$short"
if [ -e "$backup" ]; then
    saved=$(sha256sum "$backup"); saved=${saved%% *}; test "$saved" = "$actual"
else
    cp -p "$target" "$backup"
fi
app_stage="$target.thermal-new-$$"
test ! -e "$app_stage"
cp mlk_ps_local "$app_stage"
chmod 755 "$app_stage"
stopped=0
finish() {
    code=$?
    trap - 0
    if [ "$stopped" = 1 ]; then
        echo 'Update failed; restoring application and runtime link' >&2
        cp -p "$backup" "$target.thermal-rollback-$$" && mv "$target.thermal-rollback-$$" "$target"
        ln -sfn "$old_link" "$link"
        sync
        "$service" start || true
    fi
    exit "$code"
}
trap finish 0
stopped=1
"$service" stop
ln -sfn "$old_link" "$prefix/opt/mlk_fault.previous"
ln -sfn "mlk_fault-packages/$version" "$link"
mv "$app_stage" "$target"
sync
"$service" start
sleep 1
"$service" status
stopped=0
echo 'Application and temperature guard updated; model weights and thresholds unchanged'
