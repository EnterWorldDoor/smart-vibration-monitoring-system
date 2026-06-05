#!/bin/bash
# test_module.sh — EdgeVib V4L2 Virtual Video kernel module tests
# Run on Orange Pi 4 Pro: sudo bash test/test_module.sh
set -euo pipefail

MOD_NAME="edgevib_video"
DEV_NAME="/dev/edgevib-video-inject"

echo "=== Test 1: Load module ==="
sudo insmod ${MOD_NAME}.ko num_devices=2
dmesg | tail -5 | grep -q "${MOD_NAME}: loaded" && echo "[PASS] insmod" || echo "[FAIL] insmod"

echo ""
echo "=== Test 2: cdev exists ==="
sleep 0.5
test -c ${DEV_NAME} && echo "[PASS] cdev ${DEV_NAME}" || echo "[FAIL] cdev missing"

echo ""
echo "=== Test 3: V4L2 devices ==="
DEV_LIST=$(v4l2-ctl --list-devices 2>&1)
echo "$DEV_LIST"
echo "$DEV_LIST" | grep -q "EdgeVib Virtual Cam" && echo "[PASS] V4L2 devices found" || echo "[FAIL] V4L2 devices"

# Find device numbers
VIDEO_DEV0=$(v4l2-ctl --list-devices 2>&1 | grep -A1 "EdgeVib Virtual Cam (dev0)" | tail -1 | grep -oP '/dev/video\d+' || echo "")
VIDEO_DEV1=$(v4l2-ctl --list-devices 2>&1 | grep -A1 "EdgeVib Virtual Cam (dev1)" | tail -1 | grep -oP '/dev/video\d+' || echo "")
echo "  dev0=${VIDEO_DEV0:-not found}"
echo "  dev1=${VIDEO_DEV1:-not found}"

echo ""
echo "=== Test 4: V4L2 capabilities ==="
if [ -n "${VIDEO_DEV0}" ]; then
	v4l2-ctl -d "${VIDEO_DEV0}" --info | grep -q "edgevib_video" && echo "[PASS] driver name" || echo "[FAIL] driver name"
	v4l2-ctl -d "${VIDEO_DEV0}" --list-formats | grep -q "YUYV" && echo "[PASS] YUYV format" || echo "[FAIL] YUYV format"
fi

echo ""
echo "=== Test 5: Binary frame injection ==="
# Inject a test YUYV frame (640x480 = 614400 bytes of pixel data)
FRAME_W=640
FRAME_H=480
FRAME_SZ=$((FRAME_W * FRAME_H * 2))

python3 -c "
import struct, sys

# Build a solid gray YUYV frame (Y=128, U=128, V=128)
hdr = struct.pack('<5i', 0, ${FRAME_W}, ${FRAME_H}, 0x56595559, ${FRAME_SZ})
yuyv = b'\x80\x80\x80\x80' * (${FRAME_W} * ${FRAME_H} // 2)  # gray
sys.stdout.buffer.write(hdr + yuyv)
" | dd of=${DEV_NAME} bs=$((20 + FRAME_SZ)) count=1 2>/dev/null

sleep 0.2
FRAME_COUNT=$(cat /sys/devices/virtual/edgevib-video-inject/edgevib-video-inject/frame_count 2>/dev/null || echo "0")
if [ "$FRAME_COUNT" -ge 1 ]; then
	echo "[PASS] frame injected, count=${FRAME_COUNT}"
else
	echo "[FAIL] frame_count=${FRAME_COUNT}"
fi

echo ""
echo "=== Test 6: sysfs attributes ==="
cat /sys/devices/virtual/edgevib-video-inject/edgevib-video-inject/frame_count > /dev/null && echo "[PASS] frame_count readable" || echo "[FAIL] frame_count"
cat /sys/devices/virtual/edgevib-video-inject/last_frame_time_ms > /dev/null && echo "[PASS] last_frame_time_ms readable" || echo "[FAIL] last_frame_time_ms"

echo ""
echo "=== Test 7: Unload module ==="
sudo rmmod ${MOD_NAME}
sleep 0.3
test ! -c ${DEV_NAME} && echo "[PASS] rmmod (cdev removed)" || echo "[FAIL] cdev still exists"
dmesg | tail -3 | grep -q "${MOD_NAME}: unloaded" && echo "[PASS] rmmod (log confirmed)" || echo "[FAIL] rmmod log"

echo ""
echo "=== ALL TESTS COMPLETE ==="
