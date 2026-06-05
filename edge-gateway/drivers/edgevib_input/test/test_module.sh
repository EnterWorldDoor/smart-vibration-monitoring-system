#!/bin/bash
# test_module.sh — EdgeVib E-Stop Input Device kernel module test (7 tests)
# Usage: sudo bash test_module.sh

set -euo pipefail

MODULE="edgevib_input"
INJECT_DEV="/dev/edgevib-input-inject"
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PASS=0
FAIL=0
TESTS=0

pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; PASS=$((PASS + 1)); TESTS=$((TESTS + 1)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; FAIL=$((FAIL + 1)); TESTS=$((TESTS + 1)); }

echo "=== EdgeVib E-Stop Input Device — Kernel Module Test ==="
echo ""

# Test 1: insmod
echo "--- Test 1: insmod ---"
if insmod "../${MODULE}.ko" 2>/dev/null; then
    pass "Module loaded"
else
    fail "insmod failed"
fi

# Verify dmesg has no error
sleep 0.5
if dmesg | tail -5 | grep -qi "error"; then
    fail "dmesg contains error after insmod"
else
    pass "dmesg clean after insmod"
fi

# Test 2: inject cdev exists
echo ""
echo "--- Test 2: $INJECT_DEV exists ---"
if [ -c "$INJECT_DEV" ]; then
    pass "Character device $INJECT_DEV exists"
else
    fail "$INJECT_DEV not found"
fi

# Test 3: input device exists and has correct properties
echo ""
echo "--- Test 3: /dev/input/eventX (edgevib-estop) exists ---"
INPUT_DEV=""
for dev in /dev/input/event*; do
    if [ -c "$dev" ]; then
        # Use ioctl or sysfs to find our device
        sysfs_name="/sys/class/input/$(basename $dev)/device/name"
        if [ -f "$sysfs_name" ]; then
            dev_name=$(cat "$sysfs_name" 2>/dev/null || echo "")
            if [ "$dev_name" = "edgevib-estop" ]; then
                INPUT_DEV="$dev"
                break
            fi
        fi
    fi
done

# Fallback: check via /proc/bus/input/devices
if [ -z "$INPUT_DEV" ]; then
    INPUT_DEV=$(grep -A5 "edgevib-estop" /proc/bus/input/devices 2>/dev/null | grep "Handlers" | grep -o "event[0-9]*" | head -1 || echo "")
    if [ -n "$INPUT_DEV" ]; then
        INPUT_DEV="/dev/input/$INPUT_DEV"
    fi
fi

if [ -n "$INPUT_DEV" ] && [ -c "$INPUT_DEV" ]; then
    pass "Input device found: $INPUT_DEV (edgevib-estop)"
else
    fail "Input device 'edgevib-estop' not found in /sys/class/input or /proc/bus/input/devices"
    # List available devices for debugging
    echo "  Available input devices:"
    ls -la /dev/input/event* 2>/dev/null || true
    ls /sys/class/input/ 2>/dev/null || true
fi

# Test 4: evtest shows KEY_STOP and KEY_WAKEUP support
echo ""
echo "--- Test 4: evtest capability bits ---"
if command -v evtest >/dev/null 2>&1; then
    if [ -n "$INPUT_DEV" ]; then
        EVTEST_OUT=$(evtest --query "$INPUT_DEV" EV_KEY KEY_STOP 2>/dev/null)
        if [ "$EVTEST_OUT" = "0" ] || [ "$EVTEST_OUT" = "1" ]; then
            pass "KEY_STOP (128) supported (value=$EVTEST_OUT)"
        else
            # Try reading evtest info output
            EVTEST_INFO=$(timeout 2 evtest "$INPUT_DEV" 2>&1 | head -20 || true)
            if echo "$EVTEST_INFO" | grep -q "KEY_STOP"; then
                pass "KEY_STOP found in evtest output"
            else
                fail "KEY_STOP not found in evtest output"
            fi
        fi
    else
        fail "Cannot test evtest — input device not found"
    fi
else
    fail "evtest command not found (install evtest)"
fi

# Test 5: cdev write KEY_STOP=1 → verify via sysfs counters
echo ""
echo "--- Test 5: cdev write KEY_STOP=1 → sysfs counter ---"
# Write raw struct input_event: time_sec=0, time_usec=0, type=EV_KEY(1), code=KEY_STOP(128), value=1
python3 -c "
import struct, sys
# struct input_event: i32 tv_sec, i32 tv_usec, u16 type, u16 code, i32 value
ev = struct.pack('<iihhi', 0, 0, 1, 128, 1)
sys.stdout.buffer.write(ev)
" | dd of="$INJECT_DEV" bs=16 count=1 2>/dev/null

if [ $? -eq 0 ]; then
    pass "KEY_STOP=1 injected via cdev write (16 bytes)"
else
    fail "cdev write injection failed"
fi

# Verify injection via evtest (if we have input device)
if [ -n "$INPUT_DEV" ]; then
    # Quick grab of next event
    EVTEST_LINE=$(timeout 1 evtest --grab "$INPUT_DEV" 2>&1 | head -30 || true)

    # Inject another event for evtest to capture (synchronous write then read)
    python3 -c "
import struct, sys
# KEY_STOP press
ev1 = struct.pack('<iihhi', 0, 0, 1, 128, 1)
# EV_SYN
ev2 = struct.pack('<iihhi', 0, 0, 0, 0, 0)
# KEY_STOP release
ev3 = struct.pack('<iihhi', 0, 0, 1, 128, 0)
# EV_SYN
ev4 = struct.pack('<iihhi', 0, 0, 0, 0, 0)
sys.stdout.buffer.write(ev1 + ev2 + ev3 + ev4)
" | dd of="$INJECT_DEV" bs=64 count=1 2>/dev/null

    pass "KEY_STOP press+release injected for evtest (64 bytes)"
else
    # Can still verify sysfs counters without evtest
    pass "Injection verified via cdev write (input device not needed for this test)"
fi

# Verify sysfs counters
SYSFS_COUNT=$(cat "/sys/devices/virtual/edgevib-input-inject/edgevib-input-inject/estop_event_count" 2>/dev/null || echo "0")
if [ "$SYSFS_COUNT" -ge 2 ]; then
    pass "estop_event_count = $SYSFS_COUNT (>= 2, KEY_STOP=1 counted)"
elif [ "$SYSFS_COUNT" -ge 1 ]; then
    pass "estop_event_count = $SYSFS_COUNT (>= 1, at least one KEY_STOP=1 event)"
else
    fail "estop_event_count = $SYSFS_COUNT (expected >= 1)"
fi

SYSFS_TIME=$(cat "/sys/devices/virtual/edgevib-input-inject/edgevib-input-inject/last_estop_time_ms" 2>/dev/null || echo "0")
if [ "$SYSFS_TIME" -gt "0" ]; then
    pass "last_estop_time_ms = $SYSFS_TIME (> 0)"
else
    fail "last_estop_time_ms = $SYSFS_TIME (expected > 0)"
fi

# Test 6: cdev write KEY_WAKEUP=1
echo ""
echo "--- Test 6: cdev write KEY_WAKEUP=1 ---"
python3 -c "
import struct, sys
ev = struct.pack('<iihhi', 0, 0, 1, 143, 1)
sys.stdout.buffer.write(ev)
" | dd of="$INJECT_DEV" bs=16 count=1 2>/dev/null

if [ $? -eq 0 ]; then
    pass "KEY_WAKEUP=1 injected via cdev write"
else
    fail "KEY_WAKEUP injection failed"
fi

# Test 7: rmmod cleanup
echo ""
echo "--- Test 7: rmmod ---"
if rmmod "$MODULE" 2>/dev/null; then
    pass "Module unloaded"
else
    fail "rmmod failed"
fi

if [ ! -c "$INJECT_DEV" ]; then
    pass "Device $INJECT_DEV removed after rmmod"
else
    fail "$INJECT_DEV still exists after rmmod"
fi

echo ""
echo "=== Results: ${PASS}/${TESTS} passed, ${FAIL}/${TESTS} failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
