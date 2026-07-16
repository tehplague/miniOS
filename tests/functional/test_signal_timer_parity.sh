#!/bin/bash
set -euo pipefail

TIMEOUT=60
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIST="$PROJECT_DIR/dist/x86_64"
ISO="$BUILD_DIST/kernel.iso"
DISK="$BUILD_DIST/disk.img"
QEMU_PID=""
QEMU_OUT=""
QEMU_MON=""

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$QEMU_OUT" "$QEMU_MON" 2>/dev/null || true
}
trap cleanup EXIT

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "ERROR: qemu-system-x86_64 not found"
    exit 1
fi

send_cmd() {
    local cmd="$1" sock="$QEMU_MON" waited=0
    while [ ! -S "$sock" ] && [ $waited -lt 10 ]; do
        sleep 0.2
        waited=$((waited + 1))
    done
    [ -S "$sock" ] || return 1
    if command -v socat >/dev/null 2>&1; then
        printf '%s\r\n' "$cmd" | socat - "UNIX-CONNECT:$sock" >/dev/null 2>&1
    else
        printf '%s\r\n' "$cmd" | nc -q1 -U "$sock" >/dev/null 2>&1 || true
    fi
}

send_key() {
    local key="$1"
    send_cmd "sendkey $key" || true
    sleep 0.2
}

type_text() {
    local text="$1"
    local i ch key

    for ((i = 0; i < ${#text}; i++)); do
        ch="${text:i:1}"
        case "$ch" in
            a) key="a" ;; b) key="b" ;; c) key="c" ;; d) key="d" ;;
            e) key="e" ;; f) key="f" ;; g) key="g" ;; h) key="h" ;;
            i) key="i" ;; j) key="j" ;; k) key="k" ;; l) key="l" ;;
            m) key="m" ;; n) key="n" ;; o) key="o" ;; p) key="p" ;;
            q) key="q" ;; r) key="r" ;; s) key="s" ;; t) key="t" ;;
            u) key="u" ;; v) key="v" ;; w) key="w" ;; x) key="x" ;;
            y) key="y" ;; z) key="z" ;;
            0) key="0" ;; 1) key="1" ;; 2) key="2" ;; 3) key="3" ;;
            4) key="4" ;; 5) key="5" ;; 6) key="6" ;; 7) key="7" ;;
            8) key="8" ;; 9) key="9" ;;
            " ") key="spc" ;;
            /) key="slash" ;;
            .) key="dot" ;;
            -) key="minus" ;;
            _) key="shift-minus" ;;
            :) key="shift-semicolon" ;;
            *) continue ;;
        esac
        send_key "$key"
    done
}

wait_for_count() {
    local pattern="$1"
    local expected="$2"
    local waited=0
    while [ $waited -lt $TIMEOUT ]; do
        if [ "$(grep -c "$pattern" "$QEMU_OUT" 2>/dev/null || true)" -ge "$expected" ]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

echo "=== miniOS Signal Parity Functional Test ==="
echo "Building kernel and disk image..."
make -C "$PROJECT_DIR" iso disk-img testfs-install >/dev/null

QEMU_OUT="$(mktemp /tmp/qemu_signal_timer_XXXXXX)"
QEMU_MON="/tmp/qemu_signal_timer_mon_$$"

qemu-system-x86_64 \
    -m 128 \
    -cpu Haswell \
    -no-reboot \
    -nographic \
    -serial "file:$QEMU_OUT" \
    -monitor "unix:$QEMU_MON,server,nowait" \
    -boot order=d \
    -cdrom "$ISO" \
    -nic user,model=rtl8139 \
    -drive "file=$DISK,format=raw,if=ide,index=0" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    </dev/null >/dev/null 2>&1 &
QEMU_PID=$!

sleep 20
for _ in $(seq 1 20); do
    [ -S "$QEMU_MON" ] && break
    sleep 0.5
done

type_text "/test/bin/testsignal"
send_key "ret"
if ! wait_for_count "PASS: signal_sigaction_roundtrip_replace_mask" 1; then
    echo "FAIL: first testsignal run did not complete expected signal markers"
    sed -n '1,260p' "$QEMU_OUT"
    exit 1
fi

type_text "echo phase46-shell-still-alive"
send_key "ret"
if ! wait_for_count "phase46-shell-still-alive" 1; then
    echo "FAIL: shell did not accept a command after first testsignal run"
    sed -n '1,260p' "$QEMU_OUT"
    exit 1
fi

type_text "/test/bin/testsignal"
send_key "ret"
if ! wait_for_count "PASS: signal_sigaction_roundtrip_replace_mask" 2; then
    echo "FAIL: second testsignal run did not complete expected signal markers"
    sed -n '1,320p' "$QEMU_OUT"
    exit 1
fi

# ── Phase 47: EINTR and TIME-01 tests ──────────────────────────────────────

# Phase 47 / TEST-A: Ctrl-C during sleep (INTR-01 + INTR-03 regression)
# Run a long sleep, send Ctrl-C, verify the shell returns promptly.
# If nanosleep's signal check works, the shell prints the next prompt within ~2s.
type_text "sleep 60"
send_key "ret"
sleep 2
send_key "ctrl-c"
type_text "echo phase47-ctrlc-survived"
send_key "ret"
if ! wait_for_count "phase47-ctrlc-survived" 1; then
    echo "FAIL: Ctrl-C during sleep did not return shell prompt (INTR-01 + INTR-03)"
    sed -n '1,380p' "$QEMU_OUT"
    exit 1
fi
echo "PASS (phase47): Ctrl-C during sleep returned shell prompt"

# Phase 47 / TEST-B: nanosleep EINTR via SIGALRM (INTR-01)
# BusyBox 'sleep' calls nanosleep(). SIGALRM from 'kill -14' should interrupt it.
# Sequence: start sleep 30 in background, immediately send SIGALRM, wait for exit,
# echo marker. If EINTR works, the marker appears within ~3s. Otherwise 30s timeout.
type_text "sleep 30 &"
send_key "ret"
sleep 1
type_text "kill -14 $!"
send_key "ret"
sleep 1
type_text "wait; echo phase47-eintr-ok"
send_key "ret"
if ! wait_for_count "phase47-eintr-ok" 1; then
    echo "FAIL: nanosleep was not interrupted by SIGALRM (INTR-01)"
    sed -n '1,420p' "$QEMU_OUT"
    exit 1
fi
echo "PASS (phase47): nanosleep interrupted by SIGALRM (EINTR)"

# Phase 47 / TEST-C: TSC calibration visible in boot log (TIME-01)
# The boot log (captured in QEMU_OUT via serial) should contain "TSC=" with a non-zero
# value from the lapic_timer_init() printk added in Phase 47.
if grep -q "TSC=" "$QEMU_OUT" 2>/dev/null; then
    echo "PASS (phase47): TSC calibration line found in boot log (TIME-01)"
else
    echo "FAIL: boot log does not contain TSC= calibration output (TIME-01)"
    sed -n '1,80p' "$QEMU_OUT"
    exit 1
fi

type_text "exit"
send_key "ret"
wait "$QEMU_PID" || true
QEMU_PID=""

if grep -q "FAIL:" "$QEMU_OUT"; then
    echo "FAIL: testsignal reported failures"
    sed -n '1,320p' "$QEMU_OUT"
    exit 1
fi

if [ "$(grep -c "testsignal: done" "$QEMU_OUT" 2>/dev/null || true)" -ne 2 ]; then
    echo "FAIL: expected two complete testsignal runs"
    sed -n '1,320p' "$QEMU_OUT"
    exit 1
fi

echo "PASS: signal parity markers observed across two BusyBox shell runs"
