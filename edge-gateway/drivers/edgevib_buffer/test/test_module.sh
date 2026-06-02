#!/bin/bash
# test_module.sh — EdgeVib RAM Buffer Block Device Kernel Module Tests
# Run on Orange Pi: sudo bash test_module.sh
set -euo pipefail

MODULE="edgevib_buffer"
BLKDEV="/dev/edgevib-buffer"
SYSFS_BASE="/sys/devices/virtual/block/edgevib-buffer"

PASS=0
FAIL=0

green() { echo -e "\033[32m  PASS\033[0m: $1"; }
red()   { echo -e "\033[31m  FAIL\033[0m: $1"; }

echo "============================================"
echo " EdgeVib RAM Buffer Block Device Test Suite"
echo "============================================"

# Test 1: Insert module
echo ""
echo "--- Test 1: insmod $MODULE ring_size_mb=2 ---"
if sudo insmod ../${MODULE}.ko ring_size_mb=2 2>/dev/null; then
    green "Module inserted (2MB ring)"
    ((PASS++))
else
    red "insmod failed — check dmesg for errors"
    ((FAIL++))
    exit 1
fi

sleep 1

# Test 2: Block device exists
echo ""
echo "--- Test 2: $BLKDEV exists ---"
if [ -b "$BLKDEV" ]; then
    green "Block device node exists"
    ((PASS++))
else
    red "Block device missing"
    ((FAIL++))
fi

# Test 3: Sector count
echo ""
echo "--- Test 3: Sector count (2MB = 512 sectors) ---"
SECTORS=$(sudo blockdev --getsz "$BLKDEV" 2>/dev/null || echo "0")
if [ "$SECTORS" -eq 512 ]; then
    green "Sector count = $SECTORS (expected 512)"
    ((PASS++))
else
    red "Sector count = $SECTORS (expected 512)"
    ((FAIL++))
fi

# Test 4: Write + Read roundtrip
echo ""
echo "--- Test 4: Write + Read Data Consistency ---"
echo -n "EdgeVib Test $(date +%s)" | \
    sudo dd of="$BLKDEV" bs=4096 count=1 conv=notrunc 2>/dev/null
READBACK=$(sudo dd if="$BLKDEV" bs=4096 count=1 2>/dev/null | strings | head -1)
if echo "$READBACK" | grep -q "EdgeVib"; then
    green "Data roundtrip verified: $READBACK"
    ((PASS++))
else
    red "Data mismatch: got '$READBACK'"
    ((FAIL++))
fi

# Test 5: Ring overrun (write 512+10 sectors to 512-sector ring)
echo ""
echo "--- Test 5: Ring Overrun Detection ---"
sudo dd if=/dev/zero of="$BLKDEV" bs=4096 count=522 status=none 2>/dev/null
OVERRUN=$(cat "${SYSFS_BASE}/overrun_sectors" 2>/dev/null || echo "-1")
if [ "$OVERRUN" -ge 10 ] 2>/dev/null; then
    green "Overrun detected: $OVERRUN sectors overwritten (expect >= 10)"
    ((PASS++))
else
    red "Overrun count = $OVERRUN (expect >= 10)"
    ((FAIL++))
fi

# Test 6: Custom sysfs attributes
echo ""
echo "--- Test 6: Custom sysfs attributes ---"
OK=0
for attr in overrun_sectors oldest_sector_age_ms; do
    if [ -f "${SYSFS_BASE}/${attr}" ]; then
        VAL=$(cat "${SYSFS_BASE}/${attr}" 2>/dev/null)
        green "$attr = $VAL"
        ((OK++))
    else
        red "$attr missing"
    fi
done
if [ "$OK" -eq 2 ]; then
    ((PASS++))
else
    ((FAIL++))
fi

# Test 7: rmmod cleanup
echo ""
echo "--- Test 7: rmmod $MODULE ---"
sudo rmmod "$MODULE" 2>/dev/null
sleep 1
if [ ! -b "$BLKDEV" ]; then
    green "Module removed, block device cleaned up"
    ((PASS++))
else
    red "Cleanup incomplete — /dev/edgevib-buffer still exists"
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
