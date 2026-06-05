/*
 * inject.go — /dev/edgevib-input-inject write helper
 *
 * Encapsulates opening, writing struct input_event (16 bytes) in
 * little-endian format, and closing the kernel cdev injection device.
 *
 * Pattern: D5 hwmon-d/inject.go (binary struct marshal + cdev write)
 */

package main

import (
	"encoding/binary"
	"fmt"
	"os"
)

// inputEventSize is sizeof(struct input_event) on ARM64 (64-bit).
// struct timeval { long tv_sec; long tv_usec; } = 16 bytes (2×8)
// struct input_event { timeval; u16 type; u16 code; s32 value; } = 24 bytes
const inputEventSize = 24

// InputEvent mirrors the kernel's struct input_event (linux/input.h).
// ARM64 (64-bit) layout — timeval uses 8-byte longs:
//
//	Offset  Size  Field
//	 0       8    time.tv_sec  (int64)
//	 8       8    time.tv_usec (int64)
//	16       2    type          (uint16)
//	18       2    code          (uint16)
//	20       4    value         (int32)
//
// NOTE: On 32-bit platforms, sizeof(struct input_event) is 16 bytes
// because longs are 4 bytes. EdgeVib targets ARM64 only.
type InputEvent struct {
	TimeSec  int64
	TimeUsec int64
	Type     uint16 // EV_KEY=1, EV_SYN=0
	Code     uint16 // KEY_STOP=128, KEY_WAKEUP=143
	Value    int32  // 0=release, 1=press, 2=repeat
}

// Marshal serializes to 24-byte little-endian binary (ARM64 layout).
func (e *InputEvent) Marshal() []byte {
	buf := make([]byte, inputEventSize)
	binary.LittleEndian.PutUint64(buf[0:8], uint64(e.TimeSec))
	binary.LittleEndian.PutUint64(buf[8:16], uint64(e.TimeUsec))
	binary.LittleEndian.PutUint16(buf[16:18], e.Type)
	binary.LittleEndian.PutUint16(buf[18:20], e.Code)
	binary.LittleEndian.PutUint32(buf[20:24], uint32(e.Value))
	return buf
}

// Injector wraps the kernel cdev for input event injection.
type Injector struct {
	fd *os.File
}

// OpenInjector opens /dev/edgevib-input-inject (write-only).
func OpenInjector(path string) (*Injector, error) {
	fd, err := os.OpenFile(path, os.O_WRONLY, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	return &Injector{fd: fd}, nil
}

// Close closes the injection device.
func (inj *Injector) Close() error {
	return inj.fd.Close()
}

// InjectKey writes a single key event + SYN to the kernel cdev.
// Returns an error if the write fails or is short.
func (inj *Injector) InjectKey(code uint16, value int32) error {
	ev := InputEvent{
		Type:  1, // EV_KEY
		Code:  code,
		Value: value,
	}
	buf := ev.Marshal()
	n, err := inj.fd.Write(buf)
	if err != nil {
		return fmt.Errorf("write KEY %d=%d: %w", code, value, err)
	}
	if n != inputEventSize {
		return fmt.Errorf("short write: %d/%d bytes", n, inputEventSize)
	}
	return nil
}

// Sync writes an EV_SYN event to separate reports.
func (inj *Injector) Sync() error {
	ev := InputEvent{
		Type: 0, // EV_SYN
		Code: 0, // SYN_REPORT
	}
	buf := ev.Marshal()
	n, err := inj.fd.Write(buf)
	if err != nil {
		return fmt.Errorf("write SYN: %w", err)
	}
	if n != inputEventSize {
		return fmt.Errorf("short SYN write: %d/%d bytes", n, inputEventSize)
	}
	return nil
}

// InjectEStopState translates the F407 e_stop_state (0/1/2) to
// KEY_STOP/KEY_WAKEUP key events and writes them via cdev.
//
//	0 NORMAL     → KEY_STOP=0, KEY_WAKEUP=0
//	1 EMERGENCY  → KEY_STOP=1, KEY_WAKEUP=0
//	2 WAIT_RESET → KEY_STOP=0, KEY_WAKEUP=1
func (inj *Injector) InjectEStopState(state int) error {
	var keyStop, keyWakeup int32

	switch state {
	case 1: // EMERGENCY
		keyStop = 1
		keyWakeup = 0
	case 2: // WAIT_RESET
		keyStop = 0
		keyWakeup = 1
	default: // 0 NORMAL (or unknown)
		keyStop = 0
		keyWakeup = 0
	}

	if err := inj.InjectKey(128, keyStop); err != nil { // KEY_STOP
		return err
	}
	if err := inj.InjectKey(143, keyWakeup); err != nil { // KEY_WAKEUP
		return err
	}
	return inj.Sync()
}
