/*
 * buffer_io.go — /dev/edgevib-buffer block device read/write
 *
 * Serializes batch_entry structs to/from 4KB sectors.
 * Each sector = 32-byte header + up to 4064 bytes JSONB payload.
 */

package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"hash/crc32"
	"os"
	"time"
)

/* ---- Sector layout (4096 bytes) ---- */

const (
	SectorSize    = 4096
	HeaderSize    = 32
	MaxPayload    = SectorSize - HeaderSize
	MagicEDVB     = 0x56424544 // "EDVB" in LE
)

type BatchEntryHeader struct {
	Magic       uint32 // offset 0: 0x56424544
	CRC32       uint32 // offset 4: CRC32 of payload
	TimestampMs uint64 // offset 8: Unix-epoch ms
	PayloadLen  uint32 // offset 16: actual payload bytes
	DeviceCount uint32 // offset 20: devices in this batch
	BatchSeq    uint32 // offset 24: monotonic sequence number
	Reserved    uint32 // offset 28: reserved
}

type BatchEntry struct {
	Header  BatchEntryHeader
	Payload []byte // up to MaxPayload bytes
}

/* ---- BufferIO ---- */

type BufferIO struct {
	fd          *os.File
	sectorCount uint32
	writeHead   uint32
	lastReadOff uint32
}

func NewBufferIO(path string) (*BufferIO, error) {
	fd, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}

	// Determine device size
	info, err := fd.Stat()
	if err != nil {
		fd.Close()
		return nil, fmt.Errorf("stat: %w", err)
	}
	size := info.Size()
	sectorCount := uint32(size / SectorSize)

	return &BufferIO{
		fd:          fd,
		sectorCount: sectorCount,
	}, nil
}

func (b *BufferIO) Close() error {
	return b.fd.Close()
}

/* ---- Write ---- */

func (b *BufferIO) Write(entry *BatchEntry) error {
	// Serialize header
	headerBytes := make([]byte, HeaderSize)
	binary.LittleEndian.PutUint32(headerBytes[0:4], entry.Header.Magic)
	binary.LittleEndian.PutUint32(headerBytes[4:8], entry.Header.CRC32)
	binary.LittleEndian.PutUint64(headerBytes[8:16], entry.Header.TimestampMs)
	binary.LittleEndian.PutUint32(headerBytes[16:20], entry.Header.PayloadLen)
	binary.LittleEndian.PutUint32(headerBytes[20:24], entry.Header.DeviceCount)
	binary.LittleEndian.PutUint32(headerBytes[24:28], entry.Header.BatchSeq)
	binary.LittleEndian.PutUint32(headerBytes[28:32], entry.Header.Reserved)

	// Build full sector
	sector := make([]byte, SectorSize)
	copy(sector[0:HeaderSize], headerBytes)
	copy(sector[HeaderSize:HeaderSize+len(entry.Payload)], entry.Payload)

	// Calculate write position (ring)
	sectorIdx := b.writeHead % b.sectorCount
	offset := int64(sectorIdx) * SectorSize

	// Write to block device
	n, err := b.fd.WriteAt(sector, offset)
	if err != nil {
		return fmt.Errorf("write sector %d: %w", sectorIdx, err)
	}
	if n != SectorSize {
		return fmt.Errorf("short write: %d/%d", n, SectorSize)
	}

	b.writeHead++
	return nil
}

/* ---- Read ---- */

func (b *BufferIO) ReadNew(lastProcessedSeq uint32) ([]*SensorRow, error) {
	var rows []*SensorRow
	startOff := b.lastReadOff

	for off := startOff; off < b.writeHead; off++ {
		sectorIdx := off % b.sectorCount
		fileOff := int64(sectorIdx) * SectorSize

		sector := make([]byte, SectorSize)
		_, err := b.fd.ReadAt(sector, fileOff)
		if err != nil {
			return rows, fmt.Errorf("read sector %d: %w", sectorIdx, err)
		}

		// Parse header
		hdr := BatchEntryHeader{
			Magic:       binary.LittleEndian.Uint32(sector[0:4]),
			CRC32:       binary.LittleEndian.Uint32(sector[4:8]),
			TimestampMs: binary.LittleEndian.Uint64(sector[8:16]),
			PayloadLen:  binary.LittleEndian.Uint32(sector[16:20]),
			DeviceCount: binary.LittleEndian.Uint32(sector[20:24]),
			BatchSeq:    binary.LittleEndian.Uint32(sector[24:28]),
		}

		// Validate magic
		if hdr.Magic != MagicEDVB {
			continue // stale/unwritten sector
		}

		// Skip already-processed
		if hdr.BatchSeq <= lastProcessedSeq {
			continue
		}

		// Validate payload
		payload := sector[HeaderSize : HeaderSize+hdr.PayloadLen]
		expectedCRC := crc32.ChecksumIEEE(payload)
		if expectedCRC != hdr.CRC32 {
			continue // corrupted
		}

		// Parse JSONB array
		var sensorRows []*SensorRow
		if err := json.Unmarshal(payload, &sensorRows); err != nil {
			continue // invalid JSON
		}

		rows = append(rows, sensorRows...)
	}

	b.lastReadOff = b.writeHead
	return rows, nil
}

/* ---- Batch entry builder ---- */

func buildBatchEntry(msg *MQTTSensorMessage) *BatchEntry {
	ts := uint64(time.Now().UnixMilli())

	// Build JSONB payload from the MQTT message
	type jsonRow struct {
		DeviceID  string          `json:"device_id"`
		SiteID    string          `json:"site_id"`
		Timestamp int64           `json:"timestamp_ms"`
		Payload   json.RawMessage `json:"payload"`
	}
	rows := []jsonRow{{
		DeviceID:  msg.DeviceID,
		SiteID:    msg.SiteID,
		Timestamp: time.Now().UnixMilli(),
		Payload:   msg.Payload,
	}}

	payloadJSON, _ := json.Marshal(rows)

	return &BatchEntry{
		Header: BatchEntryHeader{
			Magic:       MagicEDVB,
			CRC32:       crc32.ChecksumIEEE(payloadJSON),
			TimestampMs: ts,
			PayloadLen:  uint32(len(payloadJSON)),
			DeviceCount: 1,
			BatchSeq:    ts, // use timestamp as sequence
		},
		Payload: payloadJSON,
	}
}
