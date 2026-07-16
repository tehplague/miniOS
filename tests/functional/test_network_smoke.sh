#!/bin/bash
# Functional smoke test: verify the RTL8139-backed packet path is alive.
#
# Verifies:
#   1. Guest NIC initializes and logs 'net: eth0 mac=... link=up'
#   2. Guest TX probe succeeds ('net: eth0 tx probe')
#   3. Guest classifies injected ARP  frame ('net: eth0 rx ARP frame')
#   4. Guest classifies injected IPv4 frame ('net: eth0 rx IPv4 frame')
#
# Injection mechanism: QEMU socket NIC UDP backend.
# The host Python script sends raw Ethernet frames as UDP datagrams to
# QEMU's bound socket; QEMU delivers them to the emulated RTL8139 RX ring.
# The kernel processes them during the brief poll flush in kernel_main().

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

TIMEOUT=60
NET_PORT=19000   # QEMU socket NIC localaddr (host→guest direction)
TX_PORT=19001    # QEMU socket NIC udp target (guest→host direction, ignored)

BUILD_DIST="$PROJECT_DIR/dist/x86_64"
ISO="$BUILD_DIST/kernel.iso"

QEMU_OUT=""
QEMU_PID=""
PASS=0
FAIL=0

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$QEMU_OUT" 2>/dev/null || true
}
trap cleanup EXIT

check() {
    local label="$1" pattern="$2"
    printf "  %-55s " "$label"
    if grep -q "$pattern" "$QEMU_OUT" 2>/dev/null; then
        echo "PASS"; PASS=$((PASS+1))
    else
        echo "FAIL (missing: $pattern)"; FAIL=$((FAIL+1))
    fi
}

# ── Prerequisites ──────────────────────────────────────────────────────────────

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "ERROR: qemu-system-x86_64 not found"; exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found"; exit 1
fi

echo "=== miniOS Network Smoke Test ==="
echo ""
echo "Building kernel ISO..."
if ! make -C "$PROJECT_DIR" iso 2>&1 | tail -3; then
    echo "ERROR: build failed"; exit 1
fi
echo "Build OK"
echo ""

# ── Boot QEMU with socket NIC ──────────────────────────────────────────────────

QEMU_OUT="$(mktemp /tmp/qemu_net_smoke_XXXXXX)"

echo "Starting QEMU (socket NIC udp localaddr=127.0.0.1:${NET_PORT})..."
qemu-system-x86_64 \
    -m 128 \
    -cpu Haswell \
    -no-reboot -no-shutdown \
    -nographic \
    -serial "file:$QEMU_OUT" \
    -boot order=d \
    -cdrom "$ISO" \
    -nic "socket,model=virtio-net-pci,udp=127.0.0.1:${TX_PORT},localaddr=127.0.0.1:${NET_PORT}" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    </dev/null >/dev/null 2>&1 &
QEMU_PID=$!
echo "QEMU PID=$QEMU_PID"
echo ""

# ── Inject frames after QEMU socket is ready ──────────────────────────────────
# Wait up to 5 seconds for QEMU to bind its socket, then inject.
# Frames are buffered in QEMU's emulated RTL8139 RX ring and processed
# when the kernel's poll flush runs during kernel_main().

INJECT_WAITED=0
while [ $INJECT_WAITED -lt 50 ]; do
    # Check if QEMU's UDP socket is open (ss or netstat)
    if ss -uln 2>/dev/null | grep -q ":${NET_PORT} " || \
       netstat -uln 2>/dev/null | grep -q ":${NET_PORT} " || \
       [ $INJECT_WAITED -ge 20 ]; then
        break
    fi
    sleep 0.1
    INJECT_WAITED=$((INJECT_WAITED+1))
done
sleep 0.2  # small extra margin

echo "Injecting ARP frame (ethertype 0x0806)..."
python3 "$SCRIPT_DIR/net/send_eth_frame.py" \
    --ethertype 0x0806 --host 127.0.0.1 --port "$NET_PORT" || true

echo "Injecting IPv4 frame (ethertype 0x0800)..."
python3 "$SCRIPT_DIR/net/send_eth_frame.py" \
    --ethertype 0x0800 --host 127.0.0.1 --port "$NET_PORT" || true

echo ""
echo "Frames injected. Waiting up to ${TIMEOUT}s for kernel to process them..."

# ── Wait for required log lines or timeout ────────────────────────────────────

WAITED=0
REQUIRED=("net: eth0 mac=" "net: eth0 tx probe" "net: eth0 rx ARP" "net: eth0 rx IPv4")
while [ $WAITED -lt $TIMEOUT ]; do
    ALL_FOUND=true
    for pat in "${REQUIRED[@]}"; do
        grep -q "$pat" "$QEMU_OUT" 2>/dev/null || { ALL_FOUND=false; break; }
    done
    $ALL_FOUND && break
    sleep 1
    WAITED=$((WAITED+1))
    kill -0 "$QEMU_PID" 2>/dev/null || break
done

# Kill QEMU
if kill -0 "$QEMU_PID" 2>/dev/null; then
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
fi
QEMU_PID=""

echo ""
echo "--- Results ---"
echo ""

check "NIC initialized (net: eth0 mac=)"         "net: eth0 mac="
check "Link up (link=up)"                        "link=up"
check "TX probe succeeded (net: eth0 tx probe)"  "net: eth0 tx probe"
check "ARP frame classified (net: eth0 rx ARP)"  "net: eth0 rx ARP"
check "IPv4 frame classified (net: eth0 rx IPv4)" "net: eth0 rx IPv4"

echo ""
echo "========================================"
echo "Network smoke: $PASS passed, $FAIL failed"
echo "========================================"

[ $FAIL -eq 0 ] && exit 0 || exit 1
