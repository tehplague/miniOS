#!/bin/bash
set -euo pipefail

TIMEOUT=90
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIST="$PROJECT_DIR/dist/x86_64"
ISO="$BUILD_DIST/kernel.iso"
DISK="$BUILD_DIST/disk.img"
UPSTREAM_BIN="$PROJECT_DIR/build/user/out/busybox-upstream"
STAGING_BIN="$PROJECT_DIR/targets/x86_64/testfs/bin/busybox"
STAGING_BAK="$PROJECT_DIR/targets/x86_64/testfs/bin/busybox.bak"
QEMU_PID=""
QEMU_OUT=""
QEMU_MON=""

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    # Restore original busybox binary in staging area
    if [ -f "$STAGING_BAK" ]; then
        mv "$STAGING_BAK" "$STAGING_BIN" 2>/dev/null || true
    fi
    rm -f "$QEMU_OUT" "$QEMU_MON" 2>/dev/null || true
}
trap cleanup EXIT

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

echo "=== miniOS BusyBox Upstream Regression Test ==="

if [ ! -f "$UPSTREAM_BIN" ]; then
    echo "ERROR: upstream binary not found at $UPSTREAM_BIN"
    echo "Run: make busybox-upstream"
    exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "ERROR: qemu-system-x86_64 not found"
    exit 1
fi

echo "Swapping in upstream BusyBox binary..."
# Build iso first (kernel, not disk-dependent)
make -C "$PROJECT_DIR" iso >/dev/null
# Back up patched binary, install upstream
cp "$STAGING_BIN" "$STAGING_BAK" 2>/dev/null || true
cp "$UPSTREAM_BIN" "$STAGING_BIN"
# Rebuild disk image with upstream binary in place
make -C "$PROJECT_DIR" disk-img >/dev/null
echo "Upstream binary installed. Starting QEMU..."

QEMU_OUT="$(mktemp /tmp/qemu_busybox_upstream_XXXXXX)"
QEMU_MON="/tmp/qemu_busybox_upstream_mon_$$"

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

type_text "ping -c 3 10.0.2.2"
send_key "ret"
if ! wait_for_count "3 packets received" 1; then
    echo "FAIL: upstream BusyBox ping did not complete 3 ICMP replies"
    sed -n '1,260p' "$QEMU_OUT"
    exit 1
fi
echo "PASS: upstream BusyBox ping received 3/3 ICMP replies"

type_text "echo upstream-shell-alive"
send_key "ret"
if ! wait_for_count "upstream-shell-alive" 1; then
    echo "FAIL: shell not responsive after upstream ping"
    sed -n '1,300p' "$QEMU_OUT"
    exit 1
fi
echo "PASS: shell responsive after upstream ping"

type_text "exit"
send_key "ret"
wait "$QEMU_PID" || true
QEMU_PID=""

echo ""
echo "=== BusyBox Upstream Regression: ALL CHECKS PASSED ==="
