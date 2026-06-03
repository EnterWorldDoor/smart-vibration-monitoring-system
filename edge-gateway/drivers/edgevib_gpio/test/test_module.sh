#!/bin/bash
# test_module.sh — EdgeVib Virtual GPIO Controller Module Test (7 tests)
# Run on Orange Pi: sudo bash test_module.sh
set -euo pipefail

PASS=0
FAIL=0
MODULE="edgevib_gpio"
CHIP="edgevib-gpio"

green() { ((PASS++)); echo -e "\033[32m[PASS]\033[0m $*"; }
red()   { ((FAIL++)); echo -e "\033[31m[FAIL]\033[0m $*"; }

# Ensure we start clean
if lsmod | grep -q "$MODULE"; then
    sudo rmmod "$MODULE" 2>/dev/null || true
    sleep 1
fi

echo "=== EdgeVib GPIO Module Test ==="
echo ""

# Test 1: Insert module
echo "--- Test 1: insmod ---"
if sudo insmod /opt/edge-gateway/drivers/edgevib_gpio/edgevib_gpio.ko; then
    green "Module inserted"
else
    red "Module insert failed"
    exit 1
fi

# Find the assigned chip number
CHIP_NUM=$(gpiodetect 2>/dev/null | grep "$CHIP" | head -1 | awk '{print $1}')
if [ -z "$CHIP_NUM" ]; then
    red "gpiodetect: chip '$CHIP' not found"
    exit 1
fi
CHIP_PATH="/dev/${CHIP_NUM}"
green "gpiodetect: chip found at $CHIP_PATH"

# Test 2: gpioinfo shows line names
echo ""
echo "--- Test 2: gpioinfo line names ---"
if sudo gpioinfo "$CHIP_PATH" 2>/dev/null | grep -q "unused"; then
    green "gpioinfo: lines visible (unused labels OK for virtual chip)"
else
    red "gpioinfo: no output"
fi

# Test 3: gpioset + gpioget output loopback
echo ""
echo "--- Test 3: Output set/get loopback ---"
sudo gpioset "$CHIP_PATH" 0=1
sleep 0.1
VAL=$(sudo gpioget "$CHIP_PATH" 0 2>/dev/null | tr -d '[:space:]')
if [ "$VAL" = "1" ]; then
    green "gpioset 0=1 → gpioget 0 = $VAL"
else
    red "Expected 1, got '$VAL'"
fi

sudo gpioset "$CHIP_PATH" 0=0
sleep 0.1
VAL=$(sudo gpioget "$CHIP_PATH" 0 2>/dev/null | tr -d '[:space:]')
if [ "$VAL" = "0" ]; then
    green "gpioset 0=0 → gpioget 0 = $VAL"
else
    red "Expected 0, got '$VAL'"
fi

# Test 4: inject_irq falling edge + gpiomon
echo ""
echo "--- Test 4: inject_irq falling edge ---"
SYSFS_DIR="/sys/devices/virtual/gpiochip/${CHIP}"
if [ ! -f "${SYSFS_DIR}/inject_irq" ]; then
    # Fallback: find the sysfs path via gpiochip device
    SYSFS_DIR=$(find /sys/devices -name "$CHIP" -type d 2>/dev/null | head -1)
fi

if [ -z "$SYSFS_DIR" ] || [ ! -f "${SYSFS_DIR}/inject_irq" ]; then
    red "inject_irq sysfs not found at ${SYSFS_DIR}/inject_irq"
else
    # Set up IRQ type and start gpiomon in background
    # Note: gpiomon auto-configures edge type via gpiolib
    sudo timeout 3 gpiomon -r "$CHIP_PATH" 4 > /tmp/gpiomon_test4.log 2>&1 &
    GPIOMON_PID=$!
    sleep 0.3

    # Inject falling edge (line 4, value 0)
    echo "4 0" | sudo tee "${SYSFS_DIR}/inject_irq" > /dev/null
    sleep 0.5

    # Kill gpiomon
    kill $GPIOMON_PID 2>/dev/null || true
    wait $GPIOMON_PID 2>/dev/null || true

    if grep -q "FALLING_EDGE" /tmp/gpiomon_test4.log 2>/dev/null; then
        green "inject_irq: gpiomon detected FALLING_EDGE on line 4"
    else
        red "inject_irq: gpiomon did NOT detect FALLING_EDGE on line 4"
        echo "  log: $(cat /tmp/gpiomon_test4.log 2>/dev/null || echo 'empty')"
    fi
fi

# Test 5: inject_irq rising edge + gpiomon
echo ""
echo "--- Test 5: inject_irq rising edge ---"
if [ -f "${SYSFS_DIR}/inject_irq" ]; then
    sudo timeout 3 gpiomon -r "$CHIP_PATH" 5 > /tmp/gpiomon_test5.log 2>&1 &
    GPIOMON_PID=$!
    sleep 0.3

    echo "5 1" | sudo tee "${SYSFS_DIR}/inject_irq" > /dev/null
    sleep 0.5

    kill $GPIOMON_PID 2>/dev/null || true
    wait $GPIOMON_PID 2>/dev/null || true

    if grep -q "RISING_EDGE" /tmp/gpiomon_test5.log 2>/dev/null; then
        green "inject_irq: gpiomon detected RISING_EDGE on line 5"
    else
        red "inject_irq: gpiomon did NOT detect RISING_EDGE on line 5"
        echo "  log: $(cat /tmp/gpiomon_test5.log 2>/dev/null || echo 'empty')"
    fi
else
    red "inject_irq sysfs not found — skipping test 5"
fi

# Test 6: sysfs counters
echo ""
echo "--- Test 6: sysfs counters ---"
if [ -f "${SYSFS_DIR}/irq_count" ]; then
    IRQ_CNT=$(cat "${SYSFS_DIR}/irq_count")
    if [ "$IRQ_CNT" -ge 2 ]; then
        green "irq_count = $IRQ_CNT (>= 2, expected from tests 4+5)"
    else
        red "irq_count = $IRQ_CNT, expected >= 2"
    fi

    if [ -f "${SYSFS_DIR}/last_irq_time_ms" ]; then
        LAST_MS=$(cat "${SYSFS_DIR}/last_irq_time_ms")
        if [ "$LAST_MS" -gt 0 ]; then
            green "last_irq_time_ms = $LAST_MS (> 0)"
        else
            red "last_irq_time_ms = $LAST_MS (expected > 0)"
        fi
    fi
else
    red "irq_count sysfs not found"
fi

# Test 7: rmmod cleanup
echo ""
echo "--- Test 7: rmmod ---"
if sudo rmmod "$MODULE"; then
    green "Module removed"
else
    red "rmmod failed"
    exit 1
fi

sleep 0.5
if [ ! -e "$CHIP_PATH" ]; then
    green "/dev/$CHIP_PATH removed (clean cleanup)"
else
    red "/dev/$CHIP_PATH still exists after rmmod"
fi

# Cleanup temp files
rm -f /tmp/gpiomon_test4.log /tmp/gpiomon_test5.log

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
