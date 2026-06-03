/*
 * rtc_io.go — RTC device ioctl wrapper
 *
 * Opens /dev/rtcN and performs RTC_RD_TIME / RTC_SET_TIME ioctls
 * to read/set the hardware RTC clock.
 *
 * Reference: can-d/socketcan.go for ioctl pattern (syscall.SYS_IOCTL)
 *
 * ioctl command values for ARM64 (from <linux/rtc.h>):
 *   RTC_RD_TIME  = _IOR('p', 0x09, struct rtc_time) = 0x80247009
 *   RTC_SET_TIME = _IOW('p', 0x0a, struct rtc_time) = 0x4024700a
 */

package main

import (
	"fmt"
	"syscall"
	"time"
	"unsafe"
)

/* ---- ioctl constants ---- */

const (
	RTC_RD_TIME  = 0x80247009 /* _IOR('p', 0x09, struct rtc_time) */
	RTC_SET_TIME = 0x4024700a /* _IOW('p', 0x0a, struct rtc_time) */
)

/* ---- struct rtc_time — matches kernel layout (arm64) ---- */

type rtcTime struct {
	Sec   int32
	Min   int32
	Hour  int32
	MDay  int32
	Mon   int32
	Year  int32 /* years since 1900 */
	WDay  int32
	YDay  int32
	IsDst int32
}

/* ---- RTCDevice ---- */

type RTCDevice struct {
	fd   int
	name string
}

// OpenRTC opens an RTC device and verifies it is readable.
func OpenRTC(path string) (*RTCDevice, error) {
	fd, err := syscall.Open(path, syscall.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}

	r := &RTCDevice{fd: fd, name: path}

	// Verify device is responsive by reading current time
	if _, err := r.ReadTime(); err != nil {
		syscall.Close(fd)
		return nil, fmt.Errorf("verify %s: %w", path, err)
	}

	return r, nil
}

// ReadTime reads the current RTC time.
func (r *RTCDevice) ReadTime() (time.Time, error) {
	var rt rtcTime

	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL,
		uintptr(r.fd), uintptr(RTC_RD_TIME),
		uintptr(unsafe.Pointer(&rt)))
	if errno != 0 {
		return time.Time{}, fmt.Errorf("ioctl RTC_RD_TIME: %w", errno)
	}

	// rtc_time.Tm_year is years since 1900
	tm := time.Date(
		int(rt.Year)+1900,
		time.Month(rt.Mon+1), // Tm_mon is 0-11
		int(rt.MDay),
		int(rt.Hour),
		int(rt.Min),
		int(rt.Sec),
		0,
		time.UTC,
	)

	return tm, nil
}

// SetTime sets the RTC time.
func (r *RTCDevice) SetTime(t time.Time) error {
	var rt rtcTime

	rt.Sec = int32(t.Second())
	rt.Min = int32(t.Minute())
	rt.Hour = int32(t.Hour())
	rt.MDay = int32(t.Day())
	rt.Mon = int32(t.Month() - 1) // Tm_mon is 0-11
	rt.Year = int32(t.Year() - 1900)
	rt.WDay = int32(t.Weekday())
	rt.YDay = int32(t.YearDay() - 1) // Tm_yday is 0-365
	rt.IsDst = -1                    // unknown DST

	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL,
		uintptr(r.fd), uintptr(RTC_SET_TIME),
		uintptr(unsafe.Pointer(&rt)))
	if errno != 0 {
		return fmt.Errorf("ioctl RTC_SET_TIME: %w", errno)
	}

	return nil
}

// Name returns the device path.
func (r *RTCDevice) Name() string {
	return r.name
}

// Close closes the RTC device.
func (r *RTCDevice) Close() error {
	if r.fd < 0 {
		return nil
	}
	err := syscall.Close(r.fd)
	r.fd = -1
	return err
}
