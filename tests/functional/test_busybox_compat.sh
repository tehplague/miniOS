#!/bin/bash
set -euo pipefail

TIMEOUT=90
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

echo "=== miniOS BusyBox Compatibility Regression Test ==="
echo "Building kernel and disk image..."
make -C "$PROJECT_DIR" iso disk-img testfs-install >/dev/null

QEMU_OUT="$(mktemp /tmp/qemu_busybox_compat_XXXXXX)"
QEMU_MON="/tmp/qemu_busybox_compat_mon_$$"

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

# ── Run 1 ────────────────────────────────────────────────────────────────────
type_text "ping -c 3 10.0.2.2"
send_key "ret"
if ! wait_for_count "3 packets received" 1; then
    echo "FAIL: ping run 1 did not complete 3 ICMP replies"
    sed -n '1,260p' "$QEMU_OUT"
    exit 1
fi
echo "PASS: ping run 1 completed (3/3 ICMP replies)"

type_text "echo compat-shell-alive-1"
send_key "ret"
if ! wait_for_count "compat-shell-alive-1" 1; then
    echo "FAIL: shell not responsive after ping run 1"
    sed -n '1,280p' "$QEMU_OUT"
    exit 1
fi
echo "PASS: shell responsive after ping run 1"

# ── Run 2 (socket teardown/reuse) ────────────────────────────────────────────
type_text "ping -c 3 10.0.2.2"
send_key "ret"
if ! wait_for_count "3 packets received" 2; then
    echo "FAIL: ping run 2 did not complete 3 ICMP replies"
    sed -n '1,320p' "$QEMU_OUT"
    exit 1
fi
echo "PASS: ping run 2 completed (3/3 ICMP replies)"

type_text "echo compat-shell-alive-2"
send_key "ret"
if ! wait_for_count "compat-shell-alive-2" 1; then
    echo "FAIL: shell not responsive after ping run 2"
    sed -n '1,340p' "$QEMU_OUT"
    exit 1
fi
echo "PASS: shell responsive after ping run 2"

# ── Ctrl-C mid-ping recovery ─────────────────────────────────────────────────
type_text "ping 10.0.2.2"
send_key "ret"
sleep 3
send_key "ctrl-c"
type_text "echo compat-ctrlc-survived"
send_key "ret"
if ! wait_for_count "compat-ctrlc-survived" 1; then
    echo "FAIL: Ctrl-C during ping did not return shell prompt"
    sed -n '1,380p' "$QEMU_OUT"
    exit 1
fi
echo "PASS: Ctrl-C during ping returned shell prompt"

# ── Run 3 (state clean after Ctrl-C) ─────────────────────────────────────────
type_text "ping -c 3 10.0.2.2"
send_key "ret"
if ! wait_for_count "3 packets received" 3; then
    echo "FAIL: ping run 3 did not complete 3 ICMP replies (state corrupted after Ctrl-C?)"
    sed -n '1,440p' "$QEMU_OUT"
    exit 1
fi
echo "PASS: ping run 3 completed (3/3 ICMP replies after Ctrl-C)"

type_text "echo compat-shell-alive-3"
send_key "ret"
if ! wait_for_count "compat-shell-alive-3" 1; then
    echo "FAIL: shell not responsive after ping run 3"
    sed -n '1,460p' "$QEMU_OUT"
    exit 1
fi
echo "PASS: shell responsive after ping run 3"

# ── Teardown ─────────────────────────────────────────────────────────────────
type_text "exit"
send_key "ret"
wait "$QEMU_PID" || true
QEMU_PID=""

echo ""
echo "=== BusyBox Compatibility Regression: ALL CHECKS PASSED ==="
