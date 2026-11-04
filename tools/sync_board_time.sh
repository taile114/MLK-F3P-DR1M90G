#!/usr/bin/env bash
# Set the board wall clock from the host over the debug UART. No network, no RTC required.
#
# One-shot (default): write the host UTC time once, verify
# the board epoch, exit.
#
# Periodic (--interval SECONDS): re-open and check the Linux console before
# each round, write + verify the host UTC time, then release the serial port.
# U-Boot output is receive-only so synchronization cannot cancel autoboot.
#
#   ./tools/sync_board_time.sh --interval 30            # until Ctrl-C
#   ./tools/sync_board_time.sh --interval 5 --count 3   # three syncs, then exit
#
# Failure semantics: one-shot exits non-zero on the first failure; periodic
# prints the failed round and retries. Ctrl-C in periodic mode exits 0 and
# prints the number of syncs done.
set -euo pipefail
exec python3 - "$@" <<'PY'
import argparse
import os
import re
import select
import sys
import termios
import time
from datetime import datetime, timezone

class SyncError(RuntimeError):
    pass

def fail(message):
    print(f"sync_board_time: {message}", file=sys.stderr)
    raise SystemExit(1)

def read_for(fd, seconds):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try:
                out += os.read(fd, 4096)
            except BlockingIOError:
                pass
    return out

def wait_for_session(fd, user, password, timeout):
    # A CR during U-Boot's three-second countdown cancels automatic boot. Read
    # passively through bootloader/kernel output; only probe a quiet, otherwise
    # unknown console once after five seconds, or respond to a Linux prompt.
    deadline = time.monotonic() + timeout
    last_rx = time.monotonic()
    pending = b""
    bootloader = False
    probed = False
    while time.monotonic() < deadline:
        out = read_for(fd, 0.4)
        if out:
            last_rx = time.monotonic()
            pending = (pending + out)[-8192:]
            lower = pending.lower().replace(b"\r", b"\n")
            last_boot = max(lower.rfind(b"u-boot"), lower.rfind(b"hit any key"),
                            lower.rfind(b"\n=>"))
            last_linux = max(lower.rfind(b"starting kernel"), lower.rfind(b"linux version"),
                             lower.rfind(b"welcome to buildroot"), lower.rfind(b"login:"))
            if last_boot > last_linux:
                bootloader = True
            elif last_linux >= 0:
                bootloader = False
            if bootloader:
                continue
            if re.search(rb"(?:^|\n)[^\n]*login: *$", lower):
                os.write(fd, (user + "\r").encode())
                pending = b""
                continue
            if re.search(rb"(?:^|\n)[^\n]*password: *$", lower):
                os.write(fd, (password + "\r").encode())
                pending = b""
                continue
            if re.search(rb"(?:^|\n)[^\n]*[#$] *$", lower):
                return
        if not bootloader and not probed and time.monotonic() - last_rx >= 5.0:
            os.write(fd, b"\r")
            pending = b""
            probed = True
    raise SyncError(f"no Linux root prompt on {args.device} within {timeout}s")

def open_serial(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    # Keep buffered boot output: discarding it can hide an active U-Boot prompt.
    return fd

def sync_once(fd, periodic, response_timeout=10.0):
    """Write the current host UTC time and verify the board epoch.

    Returns (stamp, host_epoch, board_epoch, delta, source) or raises SyncError.
    """
    now = datetime.now(timezone.utc)
    host_epoch = int(now.timestamp())
    set_command = (
        "if [ -x /usr/bin/mlk_set_time ]; then "
        f"mlk_set_time {host_epoch}; else date -u -s @{host_epoch}; fi"
    )
    # The marker is split in the command echo, so only the echoed result matches.
    command = (
        f"{set_command}; "
        "date -u +%s; "
        "echo __SYNC_\"END\"__\r\n"
    )
    os.write(fd, command.encode())
    data = b""
    deadline = time.time() + response_timeout
    while time.time() < deadline:
        data += read_for(fd, 0.2)
        if b"__SYNC_END__" in data:
            break
    text = data.decode("utf-8", "replace")
    if "__SYNC_END__" not in text:
        raise SyncError("no response from board after setting time")
    epochs = []
    for line in text.splitlines():
        value = line.strip()
        if value.isdigit() and len(value) >= 9:
            epochs.append(int(value))
    board_epoch = epochs[-1] if epochs else None
    if board_epoch is None:
        raise SyncError("board did not report a valid epoch")
    source = "network" if "MLK_TIME_SOURCE=network" in text.splitlines() else "serial"
    delta = abs(board_epoch - host_epoch)
    if source != "network" and delta > 5:
        raise SyncError(
            f"board time mismatch: host={host_epoch} board={board_epoch} delta={delta}s"
        )
    stamp = datetime.fromtimestamp(board_epoch, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    return stamp, host_epoch, board_epoch, delta, source

def run_once(fd):
    stamp, _, board_epoch, delta, source = sync_once(fd, periodic=False)
    print(f"board time source={source}: {stamp} UTC (epoch {board_epoch}, delta {delta}s)")

synced = 0

def connect():
    """Open the serial device and log in; raises SyncError when unavailable."""
    if not os.path.exists(args.device):
        raise SyncError(f"device not found: {args.device}")
    fd = open_serial(args.device)
    try:
        wait_for_session(fd, args.user, args.password, args.timeout)
    except BaseException:
        os.close(fd)
        raise
    return fd

def run_periodic(interval, count):
    """Service mode: re-open the port and log in again after any failure.

    The board may reboot or be unplugged between rounds, so a failure only
    closes the handle and waits for the next round; it never terminates the
    loop. Ctrl-C stops it, as does reaching --count.
    """
    global synced
    fd = None
    backoff = min(max(interval, 1.0), 5.0)
    try:
        while count == 0 or synced < count:
            try:
                if fd is None:
                    fd = connect()
                stamp, _, board_epoch, delta, source = sync_once(fd, periodic=True)
            except (SyncError, OSError) as error:
                print(f"sync_board_time: {error}", file=sys.stderr)
                if fd is not None:
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                    fd = None
                time.sleep(backoff)
                continue
            synced += 1
            # Re-check the console phase on every round. A retained Linux
            # session may have rebooted during the interval; never send date
            # blindly into a fresh U-Boot countdown.
            os.close(fd)
            fd = None
            print(
                f"sync {synced}: board time source={source}: {stamp} UTC "
                f"(epoch {board_epoch}, delta {delta}s)"
            )
            if count and synced >= count:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        pass
    finally:
        if fd is not None:
            os.close(fd)
    print(f"sync_board_time: stopped after {synced} sync(s)")

ap = argparse.ArgumentParser(
    prog="sync_board_time.sh",
    description="Set board time from host over a serial console (default /dev/ttyACM0)",
    epilog=(
        "examples: %(prog)s --interval 30 (re-sync every 30 s until Ctrl-C); "
        "%(prog)s --interval 5 --count 3 (three syncs)"
    ),
)
ap.add_argument("--device", default=os.environ.get("BOARD_TTY", "/dev/ttyACM0"))
ap.add_argument("--user", default=os.environ.get("BOARD_USER", "root"))
ap.add_argument("--password", default=os.environ.get("BOARD_PASSWORD", "root"))
ap.add_argument("--timeout", type=float, default=60.0,
                help="seconds to wait for the board root prompt")
ap.add_argument("--interval", type=float, default=0.0, metavar="SECONDS",
                help="0 = one-shot sync (default); otherwise re-sync every SECONDS")
ap.add_argument("--count", type=int, default=0, metavar="N",
                help="0 = unlimited (one-shot when --interval is 0); stop after N syncs")
args = ap.parse_args()
if args.interval < 0:
    ap.error("--interval must be >= 0")
if args.count < 0:
    ap.error("--count must be >= 0")
if not os.path.exists(args.device):
    fail(f"device not found: {args.device}")

periodic = args.interval > 0 or args.count > 0
if periodic:
    run_periodic(args.interval, args.count)
else:
    fd = connect()
    try:
        run_once(fd)
    finally:
        os.close(fd)
PY
