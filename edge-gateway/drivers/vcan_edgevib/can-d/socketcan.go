package main

import (
	"encoding/binary"
	"fmt"
	"syscall"
	"unsafe"
)

/*
 * AF_CAN constants from <linux/socket.h> and <linux/can.h>
 * Not in golang.org/x/sys/unix for all architectures, so define manually.
 */
const (
	AF_CAN  = 29
	PF_CAN  = 29
	CAN_RAW = 1

	/* ioctl */
	SIOCGIFINDEX = 0x8933
)

/* struct can_frame — matches Linux kernel <linux/can.h> layout (16 bytes on aarch64) */
type canFrame struct {
	CanID  uint32
	Len    uint8
	Flags  uint8
	Res0   uint8
	Res1   uint8
	Data   [8]uint8
}

/* struct ifreq for SIOCGIFINDEX */
type ifreq struct {
	name [16]byte
	idx  int32
}

/* struct sockaddr_can */
type sockaddrCAN struct {
	Family  uint16
	IfIndex int32
	_       [8]byte /* padding */
}

/* SocketCAN wraps an AF_CAN socket */
type SocketCAN struct {
	fd      int
	ifName  string
	ifIndex int32
}

/* NewSocketCAN opens an AF_CAN socket and binds it to the named CAN interface */
func NewSocketCAN(ifName string) (*SocketCAN, error) {
	fd, err := syscall.Socket(AF_CAN, syscall.SOCK_RAW, CAN_RAW)
	if err != nil {
		return nil, fmt.Errorf("socket(AF_CAN): %w", err)
	}

	/* Get interface index */
	var ifr ifreq
	copy(ifr.name[:], ifName)

	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd),
		SIOCGIFINDEX, uintptr(unsafe.Pointer(&ifr)))
	if errno != 0 {
		syscall.Close(fd)
		return nil, fmt.Errorf("ioctl SIOCGIFINDEX %s: %w", ifName, errno)
	}

	/* Bind socket to interface */
	addr := sockaddrCAN{
		Family:  AF_CAN,
		IfIndex: ifr.idx,
	}

	_, _, errno = syscall.Syscall(syscall.SYS_BIND, uintptr(fd),
		uintptr(unsafe.Pointer(&addr)), unsafe.Sizeof(addr))
	if errno != 0 {
		syscall.Close(fd)
		return nil, fmt.Errorf("bind %s: %w", ifName, errno)
	}

	return &SocketCAN{
		fd:      fd,
		ifName:  ifName,
		ifIndex: ifr.idx,
	}, nil
}

/* WriteFrame sends a CAN frame to the interface */
func (s *SocketCAN) WriteFrame(canID uint32, data []uint8, dlc uint8, flags uint8) error {
	if s.fd < 0 {
		return fmt.Errorf("socket not open")
	}
	if dlc > 8 {
		dlc = 8
	}

	frame := canFrame{
		CanID: canID & 0x1FFFFFFF, /* CAN_EFF_MASK */
		Len:   dlc,
		Flags: flags,
	}
	copy(frame.Data[:], data)

	/* Serialize to 16-byte buffer */
	buf := make([]byte, 16)
	binary.LittleEndian.PutUint32(buf[0:4], frame.CanID)
	buf[4] = frame.Len
	buf[5] = frame.Flags
	buf[6] = frame.Res0
	buf[7] = frame.Res1
	copy(buf[8:16], frame.Data[:])

	n, err := syscall.Write(s.fd, buf)
	if err != nil {
		return fmt.Errorf("write AF_CAN: %w", err)
	}
	if n != 16 {
		return fmt.Errorf("short write: %d/16 bytes", n)
	}

	return nil
}

/* Close releases the socket */
func (s *SocketCAN) Close() error {
	if s.fd >= 0 {
		err := syscall.Close(s.fd)
		s.fd = -1
		return err
	}
	return nil
}

/* IfName returns the CAN interface name */
func (s *SocketCAN) IfName() string {
	return s.ifName
}
