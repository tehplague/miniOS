#!/bin/bash
set -euo pipefail

TIMEOUT=30
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
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

send_cmd() {
    local cmd="$1" sock="$QEMU_MON" waited=0
    while [ ! -S "$sock" ] && [ $waited -lt 5 ]; do sleep 0.2; waited=$((waited+1)); done
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
    sleep 0.3
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
            " ") key="spc" ;;
            /) key="slash" ;;
            .) key="dot" ;;
            -) key="minus" ;;
            _) key="shift-minus" ;;
            *) continue ;;
        esac
        send_key "$key"
    done
}

echo "=== miniOS execve envp Test ==="
QEMU_OUT="$(mktemp /tmp/qemu_out_XXXXXX)"
QEMU_MON="/tmp/qemu_mon_$$"

qemu-system-x86_64 \
    -m 128 \
    -cpu Haswell \
    -no-reboot -no-shutdown \
    -nographic \
    -serial "file:$QEMU_OUT" \
    -monitor "unix:$QEMU_MON,server,nowait" \
    -boot order=d \
    -cdrom "$PROJECT_DIR/dist/x86_64/kernel.iso" \
	  -nic user,model=rtl8139 \
    -drive "file=$PROJECT_DIR/dist/x86_64/disk.img,format=raw,if=ide,index=0" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    </dev/null >/dev/null 2>&1 &
QEMU_PID=$!

echo "QEMU started (PID=$QEMU_PID) for test-env. Waiting for boot..."

# Wait for boot and monitor socket
sleep 20
for _ in $(seq 1 20); do
    [ -S "$QEMU_MON" ] && break
    sleep 0.5
done

# Type the test_env command into Busybox ash shell
type_text "/test/bin/test_env"
send_key "ret"

# Wait for test to execute
sleep 5

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "QEMU exited. Checking output..."

if grep -q "GREETING=HELLO" "$QEMU_OUT" && grep -q "SUBJECT=WORLD" "$QEMU_OUT"; then
    echo "PASS: Environment variables correctly passed to child process."
    exit 0
else
    echo "FAIL: Did not find expected environment variables in output."
    cat "$QEMU_OUT"
    exit 1
fi
