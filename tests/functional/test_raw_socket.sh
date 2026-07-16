#!/bin/bash
# Functional test: SOCK_RAW/IPPROTO_ICMP end-to-end verification.
#
# Verifies:
#   RAW-01: socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) returns valid fd
#   RAW-02: sendto() produces ICMP packet on QEMU virtual wire (pcap capture)
#   RAW-03: recvfrom() receives inbound ICMP (optional -- may timeout)
#
# Uses QEMU filter-dump to capture pcap, parsed by tshark.
# Does NOT use -nic shorthand (filter-dump requires named netdev).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TIMEOUT=60
BUILD_DIST="$PROJECT_DIR/dist/x86_64"
ISO="$BUILD_DIST/kernel.iso"
DISK="$BUILD_DIST/disk.img"

QEMU_OUT=""
QEMU_PID=""
PCAP=""
PASS=0
FAIL=0

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$QEMU_OUT" 2>/dev/null || true
    rm -f "$PCAP" 2>/dev/null || true
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

check_skip() {
    local label="$1" pattern="$2"
    printf "  %-55s " "$label"
    if grep -q "$pattern" "$QEMU_OUT" 2>/dev/null; then
        echo "PASS"; PASS=$((PASS+1))
    else
        echo "SKIP (not present -- timeout acceptable)"
    fi
}

check_pcap() {
    local label="$1"
    printf "  %-55s " "$label"
    local count
    count=$(tshark -r "$PCAP" -Y icmp 2>/dev/null | wc -l)
    if [ "$count" -gt 0 ]; then
        echo "PASS ($count ICMP packets)"; PASS=$((PASS+1))
    else
        echo "FAIL (no ICMP packets in pcap)"; FAIL=$((FAIL+1))
    fi
}

# Prerequisites
if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "ERROR: qemu-system-x86_64 not found"; exit 1
fi

if ! command -v tshark >/dev/null 2>&1; then
    echo "ERROR: tshark not found (required for pcap verification)"; exit 1
fi

echo "=== miniOS SOCK_RAW Functional Test ==="
echo ""

AUTORUN="$PROJECT_DIR/targets/x86_64/testfs/test/.autorun"
trap 'cleanup; rm -f "$AUTORUN"' EXIT

echo "Building kernel and disk image..."
if ! make -C "$PROJECT_DIR" disk-img 2>&1 | tail -5; then
    echo "ERROR: build failed"; exit 1
fi

# rawtest ELF is now built; install it as the autorun entry point so init
# execs it directly (kernel has no shebang support — must be a real ELF).
mkdir -p "$(dirname "$AUTORUN")"
ln -sf /test/bin/rawtest "$AUTORUN"

echo "Rebuilding disk image with autorun..."
if ! make -C "$PROJECT_DIR" disk-img 2>&1 | tail -3; then
    echo "ERROR: rebuild failed"; exit 1
fi
echo "Build OK"
echo ""

QEMU_OUT="$(mktemp /tmp/qemu_raw_socket_XXXXXX)"
PCAP="$(mktemp /tmp/raw_socket_pcap_XXXXXX.pcap)"

echo "Starting QEMU with filter-dump pcap capture..."
qemu-system-x86_64 \
    -m 128 \
    -cpu Haswell \
    -no-reboot -no-shutdown \
    -nographic \
    -serial "file:$QEMU_OUT" \
    -boot order=d \
    -cdrom "$ISO" \
    -drive "file=$DISK,format=raw,if=ide,index=0" \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0 \
    -object filter-dump,id=f0,netdev=net0,file="$PCAP" \
    </dev/null >/dev/null 2>&1 &
QEMU_PID=$!

echo "QEMU PID=$QEMU_PID"
echo "Waiting ${TIMEOUT}s for rawtest output..."

# Wait for rawtest: PASS (must complete fully — rawtest: sent alone does not
# guarantee the ICMP reached the wire; recvfrom's poll loop is needed to
# process the ARP reply and flush the queued packet).
WAITED=0
while [ $WAITED -lt $TIMEOUT ]; do
    if grep -q "rawtest: PASS" "$QEMU_OUT" 2>/dev/null; then
        break
    fi
    sleep 1
    WAITED=$((WAITED+1))
    kill -0 "$QEMU_PID" 2>/dev/null || break
done

# Kill QEMU and wait for exit (ensures pcap is flushed)
if kill -0 "$QEMU_PID" 2>/dev/null; then
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
fi
QEMU_PID=""

# Brief pause for pcap file flush
sleep 1

echo ""
echo "=== Results ==="
echo ""

# RAW-01: socket() succeeded
check "RAW-01: socket fd allocated (rawtest: socket fd=)" "rawtest: socket fd="

# RAW-02: sendto() succeeded
check "RAW-02: sendto succeeded (rawtest: sent)"          "rawtest: sent"

# RAW-02 wire: ICMP packet appeared on virtual wire (D-09)
check_pcap "RAW-02 wire: ICMP packet in pcap (tshark)"

# RAW-03: recvfrom received data (optional -- QEMU user-net timing variable)
check_skip "RAW-03: recvfrom received reply (rawtest: recv)"  "rawtest: recv"

echo ""
echo "========================================"
echo "SOCK_RAW functional test: $PASS passed, $FAIL failed"
echo "========================================"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
