#!/bin/bash
set -euo pipefail

TIMEOUT=60
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PASS=0
FAIL=0
QEMU_PID=""
QEMU_OUT=""
QEMU_MON=""

BUILD_DIST="$PROJECT_DIR/dist/x86_64"
ISO="$BUILD_DIST/kernel.iso"
DISK="$BUILD_DIST/disk.img"

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

echo "=== miniOS Terminal Functional Tests ==="
echo ""
echo "Building kernel and disk image..."
if ! make -C "$PROJECT_DIR" iso disk-img testfs-install 2>&1; then
    echo "ERROR: Build failed"
    exit 1
fi
echo "Build OK"
echo ""

QEMU_OUT="$(mktemp /tmp/qemu_out_XXXXXX)"
QEMU_MON="/tmp/qemu_mon_$$"
QEMU_EXIT_CODE=0

qemu-system-x86_64 \
    -m 128 \
    -cpu Haswell \
    -no-reboot -no-shutdown \
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

echo "QEMU started (PID=$QEMU_PID)"
echo ""

sleep 20
for _ in $(seq 1 20); do
    [ -S "$QEMU_MON" ] && break
    sleep 0.5
done

# Canonical Mode Test
type_text "lss"
send_key "backspace"
send_key "ret"
sleep 2

# Raw Mode Test
type_text "/test/bin/test_raw_mode"
send_key "ret"
sleep 2
send_key "a"
sleep 2

# SIGINT Test
type_text "/test/bin/test_sigint"
send_key "ret"
sleep 2
send_key "ctrl-c"
sleep 2

# BusyBox shell Ctrl-C recovery — run 1
type_text "echo phase46-shell-check-1"
send_key "ret"
sleep 2

# BusyBox shell Ctrl-C recovery — interrupt test_sigint a second time
type_text "/test/bin/test_sigint"
send_key "ret"
sleep 2
send_key "ctrl-c"
sleep 2

# BusyBox shell Ctrl-C recovery — run 2 (proves shell survived second interrupt)
type_text "echo phase46-shell-check-2"
send_key "ret"
sleep 2

# WINSIZE Test
type_text "/test/bin/test_winsize"
send_key "ret"
sleep 2

# EOF Test
send_key "ctrl-d"

WAIT=0
while kill -0 "$QEMU_PID" 2>/dev/null && [ $WAIT -lt $TIMEOUT ]; do
    sleep 1
    WAIT=$((WAIT+1))
done
if kill -0 "$QEMU_PID" 2>/dev/null; then
    kill "$QEMU_PID" 2>/dev/null || true
fi
if wait "$QEMU_PID" 2>/dev/null; then
    QEMU_EXIT_CODE=0
else
    QEMU_EXIT_CODE=$?
fi
QEMU_PID=""

# --- Verification ---

echo -n "CHECK 1: Canonical mode (echo, backspace)... "
if grep -q "lss" "$QEMU_OUT" && grep -q '$ ls' "$QEMU_OUT"; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
fi

echo -n "CHECK 2: Raw mode switch... "
if grep -q "In raw mode" "$QEMU_OUT" && grep -q "Read char: 0x61" "$QEMU_OUT"; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
fi

echo -n "CHECK 3: SIGINT (Ctrl-C)... "
if grep -q "Looping forever..." "$QEMU_OUT" && ! grep -v "Looping forever..." "$QEMU_OUT" | grep -q "Looping forever..."; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
fi

echo -n "CHECK 4: EOF (Ctrl-D)... "
if [ "$QEMU_EXIT_CODE" -eq 1 ]; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (QEMU exit code: $QEMU_EXIT_CODE, expected 1)"; FAIL=$((FAIL+1))
fi

echo -n "CHECK 5: ioctl (TIOCGWINSZ)... "
if grep -q "ws_row: 25, ws_col: 80" "$QEMU_OUT"; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
fi

echo -n "CHECK 6: Shell survived first Ctrl-C (BusyBox shell recovery)... "
if grep -q "phase46-shell-check-1" "$QEMU_OUT"; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
fi

echo -n "CHECK 7: Shell survived second Ctrl-C (repeated BusyBox recovery)... "
if grep -q "phase46-shell-check-2" "$QEMU_OUT"; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
fi

echo ""
echo "========================================"
echo "Terminal tests: $PASS passed, $FAIL failed"
echo "========================================"

[ $FAIL -eq 0 ] && exit 0 || exit 1
