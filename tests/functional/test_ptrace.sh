#!/bin/bash
# Functional test: boot miniOS and verify ptrace(2) primitives
# (PTRACE_TRACEME only, no PTRACE_ATTACH).
#
# Runs /test/bin/testptrace directly (fork + child TRACEME + execve; parent
# verifies exec-stop, PEEKDATA, SINGLESTEP, and a full PTRACE_SYSCALL trace
# to completion). See user/testptrace/main.c for the assertions.
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

# DE keyboard layout (KEYBOARD_LAYOUT_DE): '/' is Shift+7, not scancode 0x35
# ("slash" in QEMU key names maps to '-' on this build). See
# tests/functional/test_procfs.sh for the same fix.
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
            /) key="shift-7" ;;
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

echo "=== miniOS ptrace(2) Functional Test ==="

QEMU_OUT="$(mktemp /tmp/qemu_ptrace_XXXXXX)"
QEMU_MON="/tmp/qemu_ptrace_mon_$$"

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

type_text "/test/bin/testptrace"
send_key "ret"
sleep 8

if ! wait_for "PASS: ptrace_exec_stop"; then
    echo "FAIL: post-exec SIGTRAP stop not reported to tracer"
    cat "$QEMU_OUT"
    exit 1
fi
echo "PASS: exec-stop delivered to tracer"

if ! grep -q "PASS: ptrace_peekdata" "$QEMU_OUT"; then
    echo "FAIL: PTRACE_PEEKDATA at the tracee's own entry point failed"
    cat "$QEMU_OUT"
    exit 1
fi
echo "PASS: PEEKDATA read the tracee's memory correctly"

if ! grep -q "PASS: ptrace_singlestep" "$QEMU_OUT"; then
    echo "FAIL: PTRACE_SINGLESTEP did not produce exactly one more SIGTRAP stop"
    cat "$QEMU_OUT"
    exit 1
fi
echo "PASS: SINGLESTEP produced a single #DB stop"

if ! wait_for "PASS: ptrace_syscall_trace"; then
    echo "FAIL: PTRACE_SYSCALL trace to completion did not finish cleanly"
    cat "$QEMU_OUT"
    exit 1
fi
echo "PASS: PTRACE_SYSCALL traced the tracee to a clean exit"

if ! grep -q "task .* exited with code 0" "$QEMU_OUT"; then
    echo "FAIL: tracer did not exit cleanly after the trace completed"
    cat "$QEMU_OUT"
    exit 1
fi
echo "PASS: tracer exited cleanly (no post-trace corruption)"
