#!/bin/bash
# Functional test: boot miniOS in QEMU, verify interactive shell via QEMU monitor sendkey.
set -euo pipefail

TIMEOUT=45
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PASS=0
FAIL=0
QEMU_PID=""
QEMU_OUT=""
QEMU_MON=""
SMP_PID=""
SMP_OUT=""
SMP_MON=""

BUILD_DIST="$PROJECT_DIR/dist/x86_64"
ISO="$BUILD_DIST/kernel.iso"
DISK="$BUILD_DIST/disk.img"

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "$SMP_PID" ] && kill -0 "$SMP_PID" 2>/dev/null; then
        kill "$SMP_PID" 2>/dev/null || true
        wait "$SMP_PID" 2>/dev/null || true
    fi
    rm -f "$QEMU_OUT" "$QEMU_MON" "$SMP_OUT" "$SMP_MON" 2>/dev/null || true
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
            a) key="a" ;;
            b) key="b" ;;
            c) key="c" ;;
            d) key="d" ;;
            e) key="e" ;;
            f) key="f" ;;
            g) key="g" ;;
            h) key="h" ;;
            i) key="i" ;;
            j) key="j" ;;
            k) key="k" ;;
            l) key="l" ;;
            m) key="m" ;;
            n) key="n" ;;
            o) key="o" ;;
            p) key="p" ;;
            q) key="q" ;;
            r) key="r" ;;
            s) key="s" ;;
            t) key="t" ;;
            u) key="u" ;;
            v) key="v" ;;
            w) key="w" ;;
            x) key="x" ;;
            y) key="y" ;;
            z) key="z" ;;
            A) key="shift-a" ;;
            B) key="shift-b" ;;
            C) key="shift-c" ;;
            D) key="shift-d" ;;
            E) key="shift-e" ;;
            F) key="shift-f" ;;
            G) key="shift-g" ;;
            H) key="shift-h" ;;
            I) key="shift-i" ;;
            J) key="shift-j" ;;
            K) key="shift-k" ;;
            L) key="shift-l" ;;
            M) key="shift-m" ;;
            N) key="shift-n" ;;
            O) key="shift-o" ;;
            P) key="shift-p" ;;
            Q) key="shift-q" ;;
            R) key="shift-r" ;;
            S) key="shift-s" ;;
            T) key="shift-t" ;;
            U) key="shift-u" ;;
            V) key="shift-v" ;;
            W) key="shift-w" ;;
            X) key="shift-x" ;;
            Y) key="shift-y" ;;
            Z) key="shift-z" ;;
            0) key="0" ;;
            1) key="1" ;;
            2) key="2" ;;
            3) key="3" ;;
            4) key="4" ;;
            5) key="5" ;;
            6) key="6" ;;
            7) key="7" ;;
            8) key="8" ;;
            9) key="9" ;;
            " ") key="spc" ;;
            /) key="slash" ;;
            .) key="dot" ;;
            -) key="minus" ;;
            :) key="shift-semicolon" ;;
            \!) key="shift-1" ;;
            *) continue ;;
        esac
        send_key "$key"
    done
}

echo "=== miniOS Shell Functional Tests ==="
echo ""
echo "Building kernel and disk image..."
if ! make -C "$PROJECT_DIR" iso disk-img testfs-install 2>&1; then
    echo "ERROR: Build failed"
    exit 1
fi
echo "Build OK"
echo ""

# =============================================
# PART 1: Shell tests (-smp 1, default)
# =============================================

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
	  -nic user,model=virtio-net-pci \
    -drive "file=$DISK,format=raw,if=ide,index=0" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    </dev/null >/dev/null 2>&1 &
QEMU_PID=$!

echo "QEMU started (PID=$QEMU_PID)"
echo ""

# Give the kernel time to boot and reach the shell before sending input.
sleep 20
for _ in $(seq 1 20); do
    [ -S "$QEMU_MON" ] && break
    sleep 0.5
done

# CHECK 5: hello command
type_text "/test/bin/hello"
send_key "ret"
sleep 4

# CHECK 7: cat command — read /hello.txt via shell (full path needed: no arg = reads stdin)
type_text "/test/bin/cat /hello.txt"
send_key "ret"
sleep 5

# CHECK 8-12: testtty integration
type_text "/test/bin/testtty"
send_key "ret"
sleep 2
type_text "ok"
send_key "ret"
sleep 2
send_key "shift-z"
sleep 4

# CHECK 8: exit command — shell exits cleanly
type_text "exit"
send_key "ret"

# Wait for QEMU to exit via isa-debug-exit (max 60 seconds fallback)
WAIT=0
while kill -0 "$QEMU_PID" 2>/dev/null && [ $WAIT -lt 60 ]; do
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

# CHECK 1: Kernel boots
echo -n "CHECK 1: Kernel boots (Welcome to miniOS)... "
if grep -q "Welcome to miniOS" "$QEMU_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing 'Welcome to miniOS')"; FAIL=$((FAIL+1))
fi

# CHECK 2: No kernel panic
echo -n "CHECK 2: No kernel panic or fault... "
if grep -qiP "PANIC|triple fault|page fault" "$QEMU_OUT" 2>/dev/null; then
    echo "FAIL (panic detected)"; FAIL=$((FAIL+1))
else
    echo "PASS"; PASS=$((PASS+1))
fi

# CHECK 3: Shell ELF loaded
echo -n "CHECK 3: Shell ELF loaded... "
if grep -q "Shell: ELF loaded" "$QEMU_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing 'Shell: ELF loaded')"; FAIL=$((FAIL+1))
fi

# CHECK 4: Shell prompt appears
echo -n "CHECK 4: Shell prompt ('\$ ') appears... "
if grep -q '\$ ' "$QEMU_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing '$ ' prompt)"; FAIL=$((FAIL+1))
fi

# CHECK 5: hello command output
echo -n "CHECK 5: 'hello' command prints 'Hello from Newlib'... "
if grep -q "Hello from Newlib" "$QEMU_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing 'Hello from Newlib')"; FAIL=$((FAIL+1))
fi

# CHECK 6: malloc output
echo -n "CHECK 6: 'hello' command prints 'malloc OK'... "
if grep -q "malloc OK" "$QEMU_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing 'malloc OK')"; FAIL=$((FAIL+1))
fi

# CHECK 7: 'cat' command prints '/hello.txt' contents
echo -n "CHECK 7: 'cat' command prints 'Hello from miniOS disk!'... "
if grep -q "Hello from miniOS disk!" "$QEMU_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing 'Hello from miniOS disk!')"; FAIL=$((FAIL+1))
fi

# CHECK 8: exit command — shell exits cleanly (Busybox ash exits silently, no "Goodbye")
echo -n "CHECK 8: 'exit' command exits cleanly (no crash)... "
if grep -qiP "PANIC|triple fault" "$QEMU_OUT" 2>/dev/null; then
    echo "FAIL (crash after exit)"; FAIL=$((FAIL+1))
else
    echo "PASS"; PASS=$((PASS+1))
fi

# CHECK 9: No faults at end
echo -n "CHECK 9: No kernel faults at end... "
if grep -qiP "PANIC|triple fault|page fault" "$QEMU_OUT" 2>/dev/null; then
    echo "FAIL (fault or panic detected)"; FAIL=$((FAIL+1))
else
    echo "PASS"; PASS=$((PASS+1))
fi

# CHECK 10: testtty winsize
echo -n "CHECK 10: 'testtty' winsize passes... "
if grep -q "PASS: winsize" "$QEMU_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing 'PASS: winsize')"; FAIL=$((FAIL+1))
fi

# CHECK 11: testtty canonical and raw pass
echo -n "CHECK 11: 'testtty' canonical/raw pass... "
if grep -q "PASS: canonical" "$QEMU_OUT" 2>/dev/null && \
   grep -q "PASS: raw" "$QEMU_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing canonical/raw PASS markers)"; FAIL=$((FAIL+1))
fi

# CHECK 12: testtty escape path and shell prompt return
echo -n "CHECK 12: 'testtty' escape passes and shell prompt returns... "
PROMPT_COUNT="$( (grep -o '\$ ' "$QEMU_OUT" 2>/dev/null || true) | wc -l | tr -d ' ' )"
if grep -q "PASS: escape" "$QEMU_OUT" 2>/dev/null && \
   grep -q "testtty: ALL TESTS PASSED" "$QEMU_OUT" 2>/dev/null && \
   [ "${PROMPT_COUNT:-0}" -ge 2 ]; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing escape marker, summary, or returned prompt)"; FAIL=$((FAIL+1))
fi

# CHECK 14: isa-debug-exit — QEMU must exit with code 1 (qemu_exit(0) pass signal)
echo -n "CHECK 14: QEMU exited via isa-debug-exit (exit code 1)... "
if [ "$QEMU_EXIT_CODE" -eq 1 ]; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (QEMU exit code: $QEMU_EXIT_CODE, expected 1)"; FAIL=$((FAIL+1))
fi

rm -f "$QEMU_OUT" "$QEMU_MON" 2>/dev/null || true
QEMU_OUT=""
QEMU_MON=""

# =============================================
# PART 2: SMP tests (-smp 4, /bin/smptest)
# =============================================
echo ""
echo "=== SMP Functional Tests (-smp 4) ==="
echo ""

SMP_OUT="$(mktemp /tmp/qemu_smp_out_XXXXXX)"
SMP_MON="/tmp/qemu_smp_mon_$$"
SMP_EXIT_CODE=0

qemu-system-x86_64 \
    -m 128 \
    -cpu Haswell \
    -smp 4 \
    -no-reboot -no-shutdown \
    -nographic \
    -serial "file:$SMP_OUT" \
    -monitor "unix:$SMP_MON,server,nowait" \
    -boot order=d \
    -cdrom "$ISO" \
	  -nic user,model=virtio-net-pci \
    -drive "file=$DISK,format=raw,if=ide,index=0" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    </dev/null >/dev/null 2>&1 &
SMP_PID=$!

echo "QEMU -smp 4 started (PID=$SMP_PID)"
echo ""

# SMP boot needs more time: 3 APs each run delay_10ms x4 before checking in.
sleep 30
QEMU_MON="$SMP_MON"
for _ in $(seq 1 20); do
    [ -S "$SMP_MON" ] && break
    sleep 0.5
done

# Type: /test/bin/smptest + Enter
type_text "/test/bin/smptest"
send_key "ret"

# Wait for QEMU to exit via isa-debug-exit (max 180 seconds fallback)
WAIT=0
while kill -0 "$SMP_PID" 2>/dev/null && [ $WAIT -lt 180 ]; do
    sleep 1
    WAIT=$((WAIT+1))
done
if kill -0 "$SMP_PID" 2>/dev/null; then
    kill "$SMP_PID" 2>/dev/null || true
fi
if wait "$SMP_PID" 2>/dev/null; then
    SMP_EXIT_CODE=0
else
    SMP_EXIT_CODE=$?
fi
SMP_PID=""

# CHECK 10: All APs boot and check in
echo -n "CHECK 10: All 3 APs boot and check in with BSP (-smp 4)... "
if grep -q "SMP: all 3 APs checked in" "$SMP_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing 'SMP: all 3 APs checked in')"; FAIL=$((FAIL+1))
fi

# CHECK 11: smptest workers report CPU IDs
echo -n "CHECK 11: smptest workers fork and report CPU IDs... "
if grep -q "Worker PID=" "$SMP_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing 'Worker PID=')"; FAIL=$((FAIL+1))
fi

# CHECK 12: smptest completes with PASS
echo -n "CHECK 12: smptest exits with 'PASS: smptest completed'... "
if grep -q "PASS: smptest completed" "$SMP_OUT" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (missing 'PASS: smptest completed')"; FAIL=$((FAIL+1))
fi

# CHECK 13: No panics in SMP run
echo -n "CHECK 13: No kernel faults in SMP run... "
if grep -qiP "PANIC|triple fault|page fault" "$SMP_OUT" 2>/dev/null; then
    echo "FAIL (fault or panic detected)"; FAIL=$((FAIL+1))
else
    echo "PASS"; PASS=$((PASS+1))
fi

# CHECK 15: isa-debug-exit — SMP QEMU must exit with code 1
echo -n "CHECK 15: SMP QEMU exited via isa-debug-exit (exit code 1)... "
if [ "$SMP_EXIT_CODE" -eq 1 ]; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL (QEMU exit code: $SMP_EXIT_CODE, expected 1)"; FAIL=$((FAIL+1))
fi

rm -f "$SMP_OUT" "$SMP_MON" 2>/dev/null || true
SMP_OUT=""
SMP_MON=""

echo ""
echo "========================================"
echo "Functional tests: $PASS passed, $FAIL failed"
echo "========================================"

[ $FAIL -eq 0 ] && exit 0 || exit 1
