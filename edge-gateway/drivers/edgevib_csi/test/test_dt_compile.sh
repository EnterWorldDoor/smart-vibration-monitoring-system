#!/bin/bash
# test_dt_compile.sh — D7b DT overlay compile verification
# No hardware required. Run on PC or Orange Pi with dtc installed.
set -euo pipefail

DT_SRC="edgevib-csi-ov5640.dts"
DT_OUT="/tmp/edgevib-csi-ov5640.dtbo"

echo "=== Test 1: DT overlay compilation ==="
dtc -@ -I dts -O dtb -o "${DT_OUT}" "${DT_SRC}"
echo "[PASS] dtc compile"

echo ""
echo "=== Test 2: Verify compatible string ==="
dtc -I dtb -O dts "${DT_OUT}" | grep -q "ovti,ov5640" && echo "[PASS] compatible 'ovti,ov5640'" || echo "[FAIL] compatible not found"

echo ""
echo "=== Test 3: Verify I2C address ==="
dtc -I dtb -O dts "${DT_OUT}" | grep -q "reg = <0x3c>" && echo "[PASS] I2C address 0x3c" || echo "[FAIL] I2C address"

echo ""
echo "=== Test 4: Verify data-lanes ==="
dtc -I dtb -O dts "${DT_OUT}" | grep -q "data-lanes = <1 2>" && echo "[PASS] 2-lane MIPI CSI-2" || echo "[FAIL] data-lanes"

echo ""
echo "=== Test 5: Verify clock-frequency ==="
dtc -I dtb -O dts "${DT_OUT}" | grep -q "clock-frequency = <0x16e3600>" && echo "[PASS] 24MHz XCLK" || echo "[FAIL] clock-frequency"

rm -f "${DT_OUT}"
echo ""
echo "=== D7b DT OVERLAY TESTS COMPLETE ==="
echo "Note: Full sensor integration testing requires physical OV5640 MIPI CSI camera hardware."
