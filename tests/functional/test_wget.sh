#!/bin/bash
# Functional test: /bin/wget fetches an HTTP/1.0 response body from the host.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TIMEOUT=60
BUILD_DIST="$PROJECT_DIR/dist/x86_64"
ISO="$BUILD_DIST/kernel.iso"
DISK="$BUILD_DIST/disk.img"
HTTP_PORT=$((21000 + ($$ % 10000)))

QEMU_OUT=""
QEMU_PID=""
QEMU_MON=""
SERVER_DIR="/tmp/minios-wget-server"
SERVER_PID=""
PASS=0
FAIL=0

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f "$QEMU_OUT" "$QEMU_MON" 2>/dev/null || true
    rm -rf "$SERVER_DIR" 2>/dev/null || true
}
trap cleanup EXIT

send_cmd() {
    local cmd="$1" sock="$QEMU_MON" waited=0
    while [ ! -S "$sock" ] && [ $waited -lt 5 ]; do
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
            A) key="shift-a" ;; B) key="shift-b" ;; C) key="shift-c" ;; D) key="shift-d" ;;
            E) key="shift-e" ;; F) key="shift-f" ;; G) key="shift-g" ;; H) key="shift-h" ;;
            I) key="shift-i" ;; J) key="shift-j" ;; K) key="shift-k" ;; L) key="shift-l" ;;
            M) key="shift-m" ;; N) key="shift-n" ;; O) key="shift-o" ;; P) key="shift-p" ;;
            Q) key="shift-q" ;; R) key="shift-r" ;; S) key="shift-s" ;; T) key="shift-t" ;;
            U) key="shift-u" ;; V) key="shift-v" ;; W) key="shift-w" ;; X) key="shift-x" ;;
            Y) key="shift-y" ;; Z) key="shift-z" ;;
            0) key="0" ;; 1) key="1" ;; 2) key="2" ;; 3) key="3" ;; 4) key="4" ;;
            5) key="5" ;; 6) key="6" ;; 7) key="7" ;; 8) key="8" ;; 9) key="9" ;;
            " ") key="spc" ;;
            /) key="slash" ;;
            .) key="dot" ;;
            -) key="minus" ;;
            :) key="shift-semicolon" ;;
            *) continue ;;
        esac
        send_key "$key"
    done
}

check() {
    local label="$1" pattern="$2"
    printf "  %-55s " "$label"
    if grep -q "$pattern" "$QEMU_OUT" 2>/dev/null; then
        echo "PASS"; PASS=$((PASS + 1))
    else
        echo "FAIL (missing: $pattern)"; FAIL=$((FAIL + 1))
    fi
}

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "ERROR: qemu-system-x86_64 not found"; exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found"; exit 1
fi

echo "=== miniOS wget Functional Test ==="
echo ""
echo "Building kernel and disk image..."
if ! make -C "$PROJECT_DIR" disk-img 2>&1 | tail -5; then
    echo "ERROR: build failed"; exit 1
fi
echo "Build OK"
echo ""

rm -rf "$SERVER_DIR"
mkdir -p "$SERVER_DIR"
printf 'phase45-body-line\n' > "$SERVER_DIR/test.txt"
(cd "$SERVER_DIR" && python3 -m http.server "$HTTP_PORT" >/dev/null 2>&1) &
SERVER_PID=$!
sleep 1

QEMU_OUT="/tmp/minios-qemu-wget.out"
QEMU_MON="/tmp/minios-qemu-wget.mon"
rm -f "$QEMU_OUT" "$QEMU_MON"

echo "Starting QEMU (user NIC, rtl8139)..."
qemu-system-x86_64 \
    -m 128 \
    -cpu Haswell \
    -no-reboot -no-shutdown \
    -nographic \
    -serial "file:$QEMU_OUT" \
    -monitor "unix:$QEMU_MON,server,nowait" \
    -boot order=d \
    -cdrom "$ISO" \
    -drive "file=$DISK,format=raw,if=ide,index=0" \
    -nic "user,model=rtl8139" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    </dev/null >/dev/null 2>&1 &
QEMU_PID=$!

echo "QEMU PID=$QEMU_PID"
for _ in $(seq 1 40); do
    [ -S "$QEMU_MON" ] && break
    sleep 0.5
done
for _ in $(seq 1 40); do
    grep -q '\$ ' "$QEMU_OUT" 2>/dev/null && break
    sleep 1
done

type_text "wget http://10.0.2.2:$HTTP_PORT/test.txt"
send_key "ret"

WAITED=0
while kill -0 "$QEMU_PID" 2>/dev/null && [ $WAITED -lt $TIMEOUT ]; do
    if grep -q "phase45-body-line" "$QEMU_OUT" 2>/dev/null; then
        break
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done

sleep 2
if kill -0 "$QEMU_PID" 2>/dev/null; then
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
fi
QEMU_PID=""

if kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
fi
SERVER_PID=""

echo ""
echo "=== Results ==="
echo ""
check "wget printed HTTP body" "phase45-body-line"

printf "  %-55s " "wget did not print HTTP headers"
if grep -q "HTTP/1.0" "$QEMU_OUT" 2>/dev/null || grep -q "Host:" "$QEMU_OUT" 2>/dev/null; then
    echo "FAIL (header text present)"; FAIL=$((FAIL + 1))
else
    echo "PASS"; PASS=$((PASS + 1))
fi

printf "  %-55s " "No kernel panic or fault after wget run"
if grep -qiP "PANIC|triple fault|page fault" "$QEMU_OUT" 2>/dev/null; then
    echo "FAIL (fault detected)"; FAIL=$((FAIL + 1))
else
    echo "PASS"; PASS=$((PASS + 1))
fi

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "--- qemu tail ---"
    tail -n 40 "$QEMU_OUT" 2>/dev/null || true
fi

echo ""
echo "========================================"
echo "wget functional: $PASS passed, $FAIL failed"
echo "========================================"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
