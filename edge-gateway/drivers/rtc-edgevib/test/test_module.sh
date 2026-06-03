#!/bin/bash
# D4 Software RTC kernel module integration test
# Run on Orange Pi after 'make' in the kernel module directory.
# Requires: dtc, can-utils (for hwclock/rtcwake), modprobe
set -euo pipefail

MODULE="edgevib_rtc"
RTC_DEV="/dev/rtc0"
DTBO_DIR="/sys/kernel/config/device-tree/overlays"
DT_NAME="rtc-edgevib"
PLATFORM_SYSFS="/sys/devices/platform/${DT_NAME}"

PASS=0
FAIL=0

green() { ((PASS++)); echo -e "\033[32m[PASS]\033[0m $1"; }
red()   { ((FAIL++)); echo -e "\033[31m[FAIL]\033[0m $1"; }

echo "============================================"
echo "D4 Software RTC Kernel Module Test"
echo "============================================"

# ---- Clean start ----
if lsmod | grep -q "edgevib_rtc"; then
	sudo rmmod edgevib_rtc 2>/dev/null || true
	sleep 0.5
fi

# ---- Test 1: DT overlay (configfs path) ----
echo "--- Test 1: DT overlay ---"
if [ -d "$DTBO_DIR" ]; then
	sudo mkdir -p "${DTBO_DIR}/${DT_NAME}"
	if sudo sh -c "cat ../rtc-edgevib.dtbo > ${DTBO_DIR}/${DT_NAME}/dtbo" 2>/dev/null; then
		sleep 0.5
		if [ -d "$PLATFORM_SYSFS" ]; then
			green "DT overlay loaded, platform device appeared at $PLATFORM_SYSFS"
		else
			red "DT overlay applied but platform device not found at $PLATFORM_SYSFS"
		fi
	else
		echo "Note: configfs DT overlay not available — continuing with U-Boot fdtoverlays or direct insmod"
	fi
else
	echo "Note: $DTBO_DIR not found (CONFIG_OF_OVERLAY=n) — continuing without DT overlay"
fi

# ---- Test 2: insmod ----
echo "--- Test 2: insmod ---"
if sudo insmod ../edgevib_rtc.ko 2>/dev/null; then
	green "Module loaded"
else
	red "insmod failed"
	dmesg | tail -5
	exit 1
fi

sleep 0.5

# ---- Test 3: RTC standard sysfs ----
echo "--- Test 3: RTC standard sysfs ---"
RTC_CLASS=$(ls /sys/class/rtc/ 2>/dev/null | head -1)
if [ -n "$RTC_CLASS" ]; then
	if [ -f "/sys/class/rtc/$RTC_CLASS/since_epoch" ]; then
		EPOCH=$(cat "/sys/class/rtc/$RTC_CLASS/since_epoch")
		green "RTC sysfs exists: /sys/class/rtc/$RTC_CLASS/since_epoch = $EPOCH"
	else
		red "since_epoch not found in /sys/class/rtc/$RTC_CLASS"
	fi
else
	red "No RTC devices found in /sys/class/rtc/"
	ls -la /sys/class/rtc/ 2>/dev/null || true
fi

# ---- Test 4: Custom sysfs attributes ----
echo "--- Test 4: Custom sysfs attributes ---"

if [ -d "$PLATFORM_SYSFS" ]; then
	SYSFS_BASE="$PLATFORM_SYSFS"
else
	SYSFS_BASE="/sys/class/rtc/$RTC_CLASS/device"
fi

check_sysfs() {
	local attr=$1
	if [ -f "${SYSFS_BASE}/${attr}" ]; then
		VAL=$(cat "${SYSFS_BASE}/${attr}" 2>/dev/null)
		green "  $attr = $VAL"
	else
		red "  $attr not found at ${SYSFS_BASE}/${attr}"
	fi
}

check_sysfs "set_time_count"
check_sysfs "last_set_time_ms"
check_sysfs "alarm_fired_count"
check_sysfs "last_alarm_time_ms"

# ---- Test 5: hwclock --show ----
echo "--- Test 5: hwclock --show ---"
if HWCLOCK_OUT=$(sudo hwclock --show -f "$RTC_DEV" 2>&1); then
	green "hwclock read RTC: $HWCLOCK_OUT"
else
	red "hwclock --show failed: $HWCLOCK_OUT"
fi

# ---- Test 6: hwclock --systohc round-trip ----
echo "--- Test 6: hwclock --systohc → hwclock --show ---"
sudo hwclock --systohc -f "$RTC_DEV" 2>/dev/null
sleep 1
if HWCLOCK_BACK=$(sudo hwclock --show -f "$RTC_DEV" 2>&1); then
	green "hwclock systohc→show succeeded: $HWCLOCK_BACK"
else
	red "hwclock round-trip failed: $HWCLOCK_BACK"
fi

# ---- Test 7: rtcwake alarm test (3s) ----
echo "--- Test 7: rtcwake alarm (3s) ---"
ALARM_BEFORE=$(cat "${SYSFS_BASE}/alarm_fired_count" 2>/dev/null || echo 0)
if sudo rtcwake -d "$RTC_DEV" -s 3 -m no 2>/dev/null; then
	sleep 4  # Wait for alarm to fire
	ALARM_AFTER=$(cat "${SYSFS_BASE}/alarm_fired_count" 2>/dev/null || echo 0)
	if [ "$ALARM_AFTER" -gt "$ALARM_BEFORE" ]; then
		green "Alarm fired: count $ALARM_BEFORE → $ALARM_AFTER"
	else
		red "Alarm count did not increase: before=$ALARM_BEFORE after=$ALARM_AFTER"
	fi
else
	red "rtcwake failed"
fi

# ---- Test 8: hwclock --set → set_time_count +1 ----
echo "--- Test 8: hwclock --set chain ---"
COUNT_BEFORE=$(cat "${SYSFS_BASE}/set_time_count" 2>/dev/null || echo 0)
sudo hwclock --set -f "$RTC_DEV" --date "2026-01-01 00:00:00" 2>/dev/null
COUNT_AFTER=$(cat "${SYSFS_BASE}/set_time_count" 2>/dev/null || echo 0)
if [ "$COUNT_AFTER" -gt "$COUNT_BEFORE" ]; then
	green "set_time_count incremented: $COUNT_BEFORE → $COUNT_AFTER"
else
	red "set_time_count not incremented: before=$COUNT_BEFORE after=$COUNT_AFTER"
fi

# ---- Test 9: rmmod cleanup ----
echo "--- Test 9: rmmod ---"
if sudo rmmod edgevib_rtc 2>/dev/null; then
	green "Module unloaded"
else
	red "rmmod failed"
	dmesg | tail -3
fi

sleep 0.5

# Verify cleanup
if ! lsmod | grep -q "edgevib_rtc"; then
	green "Module removed from lsmod"
else
	red "Module still in lsmod"
fi

echo ""
echo "============================================"
echo "Results: $PASS passed, $FAIL failed"
echo "============================================"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
