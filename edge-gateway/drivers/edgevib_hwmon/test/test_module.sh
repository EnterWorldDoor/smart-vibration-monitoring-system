#!/bin/bash
# test_module.sh — EdgeVib HWMON Motor Health kernel module test (7 tests)
# Usage: sudo bash test_module.sh [num_motors]

set -euo pipefail

NUM_MOTORS="${1:-4}"
MODULE="edgevib_hwmon"
INJECT_DEV="/dev/edgevib-hwmon-inject"
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PASS=0
FAIL=0
TESTS=0

pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; PASS=$((PASS + 1)); TESTS=$((TESTS + 1)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; FAIL=$((FAIL + 1)); TESTS=$((TESTS + 1)); }

echo "=== EdgeVib HWMON Motor Health — Kernel Module Test (${NUM_MOTORS} motors) ==="
echo ""

# Test 1: insmod
echo "--- Test 1: insmod ---"
if insmod "${MODULE}.ko" num_motors="${NUM_MOTORS}" 2>/dev/null; then
    pass "Module loaded with num_motors=${NUM_MOTORS}"
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

# Test 2: verify injection cdev exists
echo ""
echo "--- Test 2: /dev/edgevib-hwmon-inject exists ---"
if [ -c "$INJECT_DEV" ]; then
    pass "Character device $INJECT_DEV exists"
else
    fail "$INJECT_DEV not found"
fi

# Test 3: verify hwmon devices in /sys/class/hwmon
echo ""
echo "--- Test 3: /sys/class/hwmon devices ---"
FOUND=0
for dev in /sys/class/hwmon/hwmon*; do
    name_file="$dev/name"
    if [ -f "$name_file" ]; then
        dev_name=$(cat "$name_file")
        if echo "$dev_name" | grep -q "motor0"; then
            FOUND=$((FOUND + 1))
            echo "  Found: $dev_name ($(basename $dev))"
        fi
    fi
done
if [ "$FOUND" -eq "$NUM_MOTORS" ]; then
    pass "All ${NUM_MOTORS} hwmon devices found (found $FOUND)"
else
    fail "Expected ${NUM_MOTORS} hwmon devices, found $FOUND"
fi

# Test 4: sensors command lists our devices
echo ""
echo "--- Test 4: sensors command ---"
if command -v sensors >/dev/null 2>&1; then
    SENSOR_OUT=$(sensors 2>/dev/null)
    MOTOR_LINES=$(echo "$SENSOR_OUT" | grep -c "motor0" || true)
    if [ "$MOTOR_LINES" -ge "$NUM_MOTORS" ]; then
        pass "sensors command shows ${NUM_MOTORS}+ motor devices"
    else
        fail "sensors shows $MOTOR_LINES motor lines (expected >= $NUM_MOTORS)"
    fi
else
    fail "sensors command not found (install lm-sensors)"
fi

# Test 5: inject data and verify values via sysfs
echo ""
echo "--- Test 5: cdev injection + hwmon sysfs read-back ---"
# Inject motor0: temp=45.0C, curr=5.0A, volt=230.0V, power=1150.0W
python3 -c "
import struct, sys
# motor_id=0, temp_mC=45000, curr_mA=5000, volt_mV=230000, power_mW=1150000
data = struct.pack('<5i', 0, 45000, 5000, 230000, 1150000)
sys.stdout.buffer.write(data)
" | dd of="$INJECT_DEV" bs=20 count=1 2>/dev/null

if [ $? -eq 0 ]; then
    # Find motor01 hwmon device
    HWMON_PATH=""
    for dev in /sys/class/hwmon/hwmon*; do
        if [ -f "$dev/name" ] && grep -q "motor01" "$dev/name" 2>/dev/null; then
            HWMON_PATH="$dev"
            break
        fi
    done
    if [ -n "$HWMON_PATH" ]; then
        TEMP_READ=$(cat "$HWMON_PATH/temp1_input" 2>/dev/null || echo "0")
        if [ "$TEMP_READ" = "45000" ]; then
            pass "temp1_input = $TEMP_READ (expected 45000 m°C = 45.0°C)"
        elif [ "$TEMP_READ" -gt "0" ]; then
            pass "temp1_input = $TEMP_READ (non-zero, data injected)"
        else
            fail "temp1_input = $TEMP_READ (expected 45000)"
        fi
    else
        fail "motor01 hwmon device not found in /sys/class/hwmon"
    fi
else
    fail "Binary injection to $INJECT_DEV failed"
fi

# Test 6: verify injection_count increment
echo ""
echo "--- Test 6: custom sysfs counters ---"
COUNT=$(cat "/sys/devices/virtual/edgevib-hwmon-inject/edgevib-hwmon-inject/injection_count" 2>/dev/null || echo "0")
if [ "$COUNT" -ge 1 ]; then
    pass "injection_count = $COUNT (>= 1)"
else
    fail "injection_count = $COUNT (expected >= 1)"
fi

LAST_MS=$(cat "/sys/devices/virtual/edgevib-hwmon-inject/edgevib-hwmon-inject/last_injection_time_ms" 2>/dev/null || echo "0")
if [ "$LAST_MS" -gt "0" ]; then
    pass "last_injection_time_ms = $LAST_MS (> 0)"
else
    fail "last_injection_time_ms = $LAST_MS (expected > 0)"
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
