#!/bin/bash
# Functional test: boot miniOS and verify /proc/<pid>/* files work.
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
            /) key="shift-7" ;;  # DE keyboard layout (KEYBOARD_LAYOUT_DE): '/' is Shift+7, not scancode 0x35
            .) key="dot" ;;
            -) key="minus" ;;
            _) key="shift-minus" ;;
            :) key="shift-semicolon" ;;
            *) continue ;;
        esac
        send_key "$key"
    done
}

wait_for() {
    local pattern="$1"
    local waited=0
    while [ $waited -lt $TIMEOUT ]; do
        grep -q "$pattern" "$QEMU_OUT" 2>/dev/null && return 0
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

echo "=== miniOS procfs (/proc/<pid>) Functional Test ==="

QEMU_OUT="$(mktemp /tmp/qemu_procfs_XXXXXX)"
QEMU_MON="/tmp/qemu_procfs_mon_$$"

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
    </dev/null >/dev/null 2>&1 &
QEMU_PID=$!

echo "Waiting for shell prompt..."
sleep 20
for _ in $(seq 1 20); do
    [ -S "$QEMU_MON" ] && break
    sleep 0.5
done

# cat /proc/1/status — init is always pid 1
type_text "cat /proc/1/status"
send_key "ret"
sleep 1

if ! wait_for "Name:"; then
    echo "FAIL: /proc/1/status did not produce expected output"
    cat "$QEMU_OUT"
    exit 1
fi
echo "PASS: /proc/1/status readable"

type_text "cat /proc/1/cmdline"
send_key "ret"
sleep 1

type_text "ls /proc"
send_key "ret"
sleep 1

if ! wait_for "meminfo"; then
    echo "FAIL: /proc listing did not include meminfo"
    cat "$QEMU_OUT"
    exit 1
fi
echo "PASS: /proc readdir works"

if grep -qE "ENOSYS|not implemented|task.*exit.*code 1|EXCEPTION" "$QEMU_OUT"; then
    echo "FAIL: unexpected error/exception in output"
    grep -E "ENOSYS|not implemented|EXCEPTION" "$QEMU_OUT" || true
    exit 1
fi

echo "--- procfs output ---"
grep -A5 "Name:" "$QEMU_OUT" | head -10
