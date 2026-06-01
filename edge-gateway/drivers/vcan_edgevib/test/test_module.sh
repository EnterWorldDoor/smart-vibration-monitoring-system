#!/bin/bash
# D1 Virtual CAN kernel module integration test
# Run on Orange Pi after 'make' in the kernel module directory
set -euo pipefail

MODULE="vcan_edgevib"
INTERFACE="vcan_edgevib"
PASS=0
FAIL=0

green() { echo -e "\033[32m[PASS]\033[0m $1"; PASS=$((PASS+1)); }
red()   { echo -e "\033[31m[FAIL]\033[0m $1"; FAIL=$((FAIL+1)); }

echo "=== D1 Virtual CAN Kernel Module Test ==="

# Test 1: Load module
echo "--- Test 1: insmod ---"
if sudo insmod ../vcan_edgevib.ko 2>/dev/null; then
    green "Module loaded"
else
    red "insmod failed (already loaded? try 'sudo rmmod $MODULE' first)"
    exit 1
fi

# Test 2: Interface exists
echo "--- Test 2: Interface exists ---"
if ip link show $INTERFACE >/dev/null 2>&1; then
    green "Interface $INTERFACE exists"
else
    red "Interface $INTERFACE not found"
fi

# Test 3: Bring interface up
echo "--- Test 3: ip link set up ---"
sudo ip link set $INTERFACE up
if ip link show $INTERFACE | grep -q UP; then
    green "Interface UP"
else
    red "Interface not UP"
fi

# Test 4: cansend + candump round-trip
echo "--- Test 4: cansend -> candump ---"
timeout 3 candump $INTERFACE > /tmp/candump_test.log 2>&1 &
CANDUMP_PID=$!
sleep 0.5
cansend $INTERFACE 201#AABBCCDDEEFF0011
cansend $INTERFACE 202#DEADBEEF
sleep 1
sudo kill $CANDUMP_PID 2>/dev/null || true
wait $CANDUMP_PID 2>/dev/null || true

if grep -q "201.*AA BB CC DD EE FF" /tmp/candump_test.log; then
    green "cansend -> candump: frame 0x201 received correctly"
else
    red "cansend -> candump: frame 0x201 not received"
    cat /tmp/candump_test.log
fi

# Test 5: sysfs attributes
echo "--- Test 5: sysfs attributes ---"
CRC_PATH="/sys/class/net/$INTERFACE/device/crc_errors"
FIFO_PATH="/sys/class/net/$INTERFACE/device/fifo_overruns"
if [ -f "$CRC_PATH" ]; then
    CRC_VAL=$(cat "$CRC_PATH")
    green "crc_errors sysfs exists (value=$CRC_VAL)"
else
    red "crc_errors sysfs not found at $CRC_PATH"
fi
if [ -f "$FIFO_PATH" ]; then
    FIFO_VAL=$(cat "$FIFO_PATH")
    green "fifo_overruns sysfs exists (value=$FIFO_VAL)"
else
    red "fifo_overruns sysfs not found at $FIFO_PATH"
fi

# Test 6: stats counters
echo "--- Test 6: netdev stats ---"
STATS=$(ip -s link show $INTERFACE)
TX_PKTS=$(echo "$STATS" | grep -A1 TX | tail -1 | awk '{print $1}')
if [ "$TX_PKTS" -ge 2 ]; then
    green "TX packets >= 2 (actual=$TX_PKTS)"
else
    red "TX packets < 2 (actual=$TX_PKTS)"
fi

# Test 7: Unload module
echo "--- Test 7: rmmod ---"
sudo ip link set $INTERFACE down
if sudo rmmod $MODULE 2>/dev/null; then
    green "Module unloaded"
else
    red "rmmod failed"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ] && exit 0 || exit 1
