#!/bin/bash
# test_module.sh — EdgeVib IIO Vibration Driver Kernel Module Tests
# Run on Orange Pi: sudo bash test_module.sh
set -euo pipefail

MODULE="edgevib_iio"
INJECT_DEV="/dev/edgevib-iio-inject"
IIO_DEV="/sys/bus/iio/devices"

PASS=0
FAIL=0

green() { echo -e "\033[32m  PASS\033[0m: $1"; }
red()   { echo -e "\033[31m  FAIL\033[0m: $1"; }

echo "============================================"
echo " EdgeVib IIO Vibration Module Test Suite"
echo "============================================"

# Test 1: Insert module
echo ""
echo "--- Test 1: insmod $MODULE ---"
if sudo insmod ../${MODULE}.ko 2>/dev/null; then
    green "Module inserted successfully"
    ((PASS++))
else
    red "insmod failed — check dmesg for errors"
    ((FAIL++))
    exit 1
fi

# Test 2: Verify injection device node
echo ""
echo "--- Test 2: $INJECT_DEV exists ---"
sleep 1
if [ -c "$INJECT_DEV" ]; then
    green "Inject device node exists"
    ((PASS++))
else
    red "Inject device node missing"
    ((FAIL++))
fi

# Test 3: Verify IIO device registered
echo ""
echo "--- Test 3: IIO device registered ---"
if [ -d "$IIO_DEV" ] && ls "$IIO_DEV"/iio:device* >/dev/null 2>&1; then
    green "IIO device found: $(ls $IIO_DEV/iio:device*)"
    ((PASS++))
else
    red "No IIO device found in $IIO_DEV"
    ((FAIL++))
fi

# Test 4: Verify 24 channels via iio_info (if available)
echo ""
echo "--- Test 4: IIO channel count ---"
IIODEV=$(ls "$IIO_DEV"/iio:device* 2>/dev/null | head -1)
if [ -n "$IIODEV" ] && [ -d "$IIODEV/scan_elements" ]; then
    CHAN_COUNT=$(ls "$IIODEV/scan_elements"/in_*_en 2>/dev/null | wc -l)
    if [ "$CHAN_COUNT" -eq 24 ]; then
        green "24 scan elements found"
        ((PASS++))
    else
        red "Expected 24 scan elements, found $CHAN_COUNT"
        ((FAIL++))
    fi
else
    red "Cannot determine channel count (no scan_elements)"
    ((FAIL++))
fi

# Test 5: Data injection via cdev
echo ""
echo "--- Test 5: Inject 96 bytes ---"
# Generate 96 bytes: 24 × float32(1.0) = 24 × (0x0000803F in LE)
python3 -c "
import struct, sys
data = struct.pack('<24f', *[float(i) for i in range(24)])
sys.stdout.buffer.write(data)
" | sudo dd of="$INJECT_DEV" bs=96 count=1 2>/dev/null
if [ $? -eq 0 ]; then
    green "96-byte injection succeeded"
    ((PASS++))
else
    red "Injection failed"
    ((FAIL++))
fi

# Test 6: Verify injection counter incremented
echo ""
echo "--- Test 6: injection_count sysfs ---"
COUNT_FILE="$IIODEV/injection_count"
if [ -f "$COUNT_FILE" ]; then
    COUNT=$(cat "$COUNT_FILE")
    if [ "$COUNT" -ge 1 ]; then
        green "injection_count = $COUNT (expect >= 1)"
        ((PASS++))
    else
        red "injection_count = $COUNT (expect >= 1)"
        ((FAIL++))
    fi
else
    red "injection_count sysfs missing"
    ((FAIL++))
fi

# Test 7: Cleanup — rmmod
echo ""
echo "--- Test 7: rmmod $MODULE ---"
sudo rmmod "$MODULE" 2>/dev/null
if [ ! -c "$INJECT_DEV" ] && [ ! -d "$IIO_DEV/iio:device0" ]; then
    green "Module removed, devices cleaned up"
    ((PASS++))
else
    red "Cleanup incomplete — devices may still exist"
    ((FAIL++))
fi

echo ""
echo "============================================"
echo " Results: $PASS passed, $FAIL failed"
echo "============================================"

if [ "$FAIL" -eq 0 ]; then
    echo "All tests passed!"
    exit 0
else
    echo "Some tests failed — check dmesg for details"
    exit 1
fi
