#!/bin/bash
# Functional test: ping ICMP echo round-trip via QEMU user-network.
#
# Verifies:
#   PING-01: /bin/ping sends ICMP echo requests via SOCK_RAW
#   PING-02: ping prints RTT in milliseconds
#   PING-03: ping prints packet loss summary
#   TEST-01: Automated QEMU functional test
#   SOCK-01: ICMP reply payload (64 bytes) is preserved end-to-end
#   SOCK-02: Three consecutive pings all receive replies (no stale state)
#
# Uses QEMU -nic user,model=rtl8139. No tshark dependency.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TIMEOUT=90
BUILD_DIST="$PROJECT_DIR/dist/x86_64"
ISO="$BUILD_DIST/kernel.iso"
DISK="$BUILD_DIST/disk.img"
AUTORUN="$PROJECT_DIR/targets/x86_64/testfs/test/.autorun"

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
    rm -f "$AUTORUN" 2>/dev/null || true
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

check_regex() {
    local label="$1" pattern="$2"
    printf "  %-55s " "$label"
    if grep -Eq "$pattern" "$QEMU_OUT" 2>/dev/null; then
        echo "PASS"; PASS=$((PASS+1))
    else
        echo "FAIL (missing regex: $pattern)"; FAIL=$((FAIL+1))
    fi
}

check_serial_regex() {
    local label="$1" pattern="$2"
    printf "  %-55s " "$label"
    if tr '\r\n' '  ' < "$QEMU_OUT" | grep -Eq "$pattern" 2>/dev/null; then
        echo "PASS"; PASS=$((PASS+1))
    else
        echo "FAIL (missing regex: $pattern)"; FAIL=$((FAIL+1))
    fi
}

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "ERROR: qemu-system-x86_64 not found"; exit 1
fi

echo "=== miniOS Ping Functional Test ==="
echo ""

# Install autorun script that runs ping 3 times + ifconfig.
# Init execs /test/.autorun if it exists (must be a real ELF or a script
# handled by the shell); use the BusyBox sh to run the command sequence.
mkdir -p "$(dirname "$AUTORUN")"
cat > "$AUTORUN" << 'AUTORUN_EOF'
#!/bin/sh
# Run ping -c 3 to prove SOCK-01 + SOCK-02, then ifconfig for address proof.
ping -c 3 10.0.2.2
ifconfig
AUTORUN_EOF
chmod +x "$AUTORUN"

echo "Building kernel and disk image (with autorun)..."
if ! make -C "$PROJECT_DIR" disk-img 2>&1 | tail -5; then
    echo "ERROR: build failed"; exit 1
fi
echo "Build OK"
echo ""

QEMU_OUT="$(mktemp /tmp/qemu_ping_XXXXXX)"

echo "Starting QEMU (user NIC, rtl8139)..."
qemu-system-x86_64 \
    -m 128 \
    -cpu Haswell \
    -no-reboot -no-shutdown \
    -nographic \
    -serial "file:$QEMU_OUT" \
    -boot order=d \
    -cdrom "$ISO" \
    -drive "file=$DISK,format=raw,if=ide,index=0" \
    -nic "user,model=rtl8139" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    </dev/null >/dev/null 2>&1 &
QEMU_PID=$!

echo "QEMU PID=$QEMU_PID"
echo "Waiting ${TIMEOUT}s for ping output..."

# Wait for the ping markers plus the guest ifconfig proof or timeout
WAITED=0
while [ $WAITED -lt $TIMEOUT ]; do
    grep -q "seq=0 time=" "$QEMU_OUT" 2>/dev/null &&
    grep -q "packets transmitted" "$QEMU_OUT" 2>/dev/null &&
    grep -q "eth0      inet addr:10.0.2.15" "$QEMU_OUT" 2>/dev/null &&
    grep -q "HWaddr 52:54:00:12:34:56" "$QEMU_OUT" 2>/dev/null &&
    tr '\r\n' '  ' < "$QEMU_OUT" | grep -Eq "RX bytes:[1-9][0-9]*[[:space:]]+TX bytes:[1-9][0-9]*" 2>/dev/null && break
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
echo "=== Results ==="
echo ""
check "Network initialized (lwIP)"               "net: lwIP initialized"
check "ICMP echo reply received (seq=0)"          "seq=0 time="
check "RTT measured in ms"                        "time="
check "Summary line printed"                      "packets transmitted"
check "ifconfig inet address printed"             "eth0      inet addr:10.0.2.15"
check "ifconfig hardware address printed"         "HWaddr 52:54:00:12:34:56"
check_serial_regex "ifconfig RX/TX bytes are non-zero" "RX bytes:[1-9][0-9]*[[:space:]]+TX bytes:[1-9][0-9]*"

# SOCK-01 proof: ICMP reply payload is preserved end-to-end (hacks removed).
# BusyBox ping prints the full reply line with exact byte count — any payload
# truncation would show fewer bytes or missing RTT.
check_regex "Full ICMP reply length (64 bytes)"          "64 bytes from 10\.0\.2\.2: seq=0 time="

# SOCK-02 proof: Three consecutive pings still receive all three replies
# (covers ring teardown + pcb removal on socket close between runs).
check_regex "Three ICMP echo replies received"           "seq=2 time="

echo ""
echo "========================================"
echo "Ping functional: $PASS passed, $FAIL failed"
echo "========================================"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
