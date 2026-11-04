sh <<'F3P_TIMEOUT_INSTALL'
set -eu
trap 'sync' 0
bundle=${F3P_LINEAR_DIR:-/mnt/mmcblk0p2/npu-tests/linear-v1-19700101_002235-357/f3p-npu-linear-v1}
cd "$bundle"
test -x bin/npu_linear
sha256sum -c checksums.sha256 > timeout-install-precheck.log || {
    cat timeout-install-precheck.log
    exit 1
}
compat="$bundle/compat_timeout_v1"
if [ ! -e "$compat" ]; then
    mkdir "$compat"
    base64 -d <<'F3P_TIMEOUT_PAYLOAD' | gzip -dc > "$compat/timeout"
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
F3P_TIMEOUT_PAYLOAD
fi
printf '%s  %s\n' 2b5fe6bdc7c7efb6528e490f58c3e098b020a557f4539cbb89357e820090d248 "$compat/timeout" | sha256sum -c -
chmod +x "$compat/timeout"
if [ ! -e run_compat.sh ]; then
    cat > run_compat.sh <<'F3P_COMPAT_WRAPPER'
#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test -x "$root/compat_timeout_v1/timeout" || { echo 'Missing timeout compatibility helper' >&2; exit 2; }
export PATH="$root/compat_timeout_v1:$PATH"
exec sh "$root/run.sh" "$@"
F3P_COMPAT_WRAPPER
fi
printf '%s  %s\n' eefb71bebfc9a13cdc22c876f67048ed358828809a90ac1678f8c0b30b13d01f run_compat.sh | sha256sum -c -
"$compat/timeout" -k 1 2 /bin/sh -c 'exit 0'
status=0
"$compat/timeout" -k 1 2 /bin/sh -c 'exit 40' || status=$?
test "$status" -eq 40
status=0
"$compat/timeout" -k 1 1 /bin/sh -c 'sleep 10' || status=$?
test "$status" -eq 124
echo 'timeout_self_checks=passed'
echo "test_directory=$PWD"
sh run_compat.sh control
F3P_TIMEOUT_INSTALL
