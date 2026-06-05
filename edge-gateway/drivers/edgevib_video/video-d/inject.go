package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"log/slog"
	"os"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	injectHeaderSize   = 20 // 5 × int32
	v4l2PixFmtYUYV     = 0x56595559 // V4L2_PIX_FMT_YUYV little-endian
)

// VideoInjection matches the kernel's struct video_inject_header + variable data
type VideoInjection struct {
	DeviceID    int32
	Width       int32
	Height      int32
	PixelFormat uint32
	DataSize    int32
	Data        []byte // YUYV raw pixels
}

// Marshal serializes to binary: 20-byte header + variable YUYV payload
func (v *VideoInjection) Marshal() []byte {
	buf := make([]byte, injectHeaderSize+len(v.Data))
	binary.LittleEndian.PutUint32(buf[0:4], uint32(v.DeviceID))
	binary.LittleEndian.PutUint32(buf[4:8], uint32(v.Width))
	binary.LittleEndian.PutUint32(buf[8:12], uint32(v.Height))
	binary.LittleEndian.PutUint32(buf[12:16], v.PixelFormat)
	binary.LittleEndian.PutUint32(buf[16:20], uint32(v.DataSize))
	copy(buf[injectHeaderSize:], v.Data)
	return buf
}

// InjectionStats tracks daemon health metrics
type InjectionStats struct {
	Injections    int64
	DBErrors      int64
	InjectErrors  int64
	SkipNoFile    int64
	SkipDuplicate int64
	LastPollMS    int64
}

// jpegToYUYV decodes a JPEG byte slice and converts to YUYV 4:2:2 format.
// Returns the YUYV packed bytes, width, and height.
func jpegToYUYV(jpegData []byte) ([]byte, int, int, error) {
	img, err := jpeg.Decode(bytes.NewReader(jpegData))
	if err != nil {
		return nil, 0, 0, fmt.Errorf("jpeg decode: %w", err)
	}

	bounds := img.Bounds()
	w := bounds.Dx()
	h := bounds.Dy()

	// YUYV packed format: [Y0, U, Y1, V] per 2 pixels, 2 bytes/pixel average
	yuyv := make([]byte, w*h*2)

	for y := 0; y < h; y++ {
		for x := 0; x < w; x += 2 {
			// Get RGB for two adjacent pixels
			r0, g0, b0, _ := img.At(bounds.Min.X+x, bounds.Min.Y+y).RGBA()
			r1, g1, b1, _ := img.At(bounds.Min.X+x+1, bounds.Min.Y+y).RGBA()

			// RGB to YCbCr (BT.601, 8-bit)
			y0, cb0, cr0 := rgbToYCbCr(uint8(r0>>8), uint8(g0>>8), uint8(b0>>8))
			y1, cb1, cr1 := rgbToYCbCr(uint8(r1>>8), uint8(g1>>8), uint8(b1>>8))

			// Average chroma from both pixels (4:2:2 subsampling)
			cb := uint8((uint16(cb0) + uint16(cb1)) / 2)
			cr := uint8((uint16(cr0) + uint16(cr1)) / 2)

			offset := (y*w + x) * 2
			yuyv[offset+0] = y0
			yuyv[offset+1] = cb // U
			yuyv[offset+2] = y1
			yuyv[offset+3] = cr // V
		}
	}

	return yuyv, w, h, nil
}

// rgbToYCbCr converts RGB (8-bit) to YCbCr (8-bit) using BT.601 full-range.
func rgbToYCbCr(r, g, b uint8) (y, cb, cr uint8) {
	// Y  =  0.299*R + 0.587*G + 0.114*B
	// Cb = -0.169*R - 0.331*G + 0.500*B + 128
	// Cr =  0.500*R - 0.419*G - 0.081*B + 128
	rf, gf, bf := float64(r), float64(g), float64(b)

	y  = uint8(0.299*rf + 0.587*gf + 0.114*bf + 0.5)
	cb = uint8(-0.169*rf - 0.331*gf + 0.500*bf + 128.5)
	cr = uint8(0.500*rf - 0.419*gf - 0.081*bf + 128.5)
	return
}

// jpegToYUYVFast is an optimized version for *image.YCbCr input.
// Most JPEGs decode to YCbCr natively, avoiding RGB roundtrip.
func jpegToYUYVFast(ycbcr *image.YCbCr) ([]byte, int, int) {
	w := ycbcr.Bounds().Dx()
	h := ycbcr.Bounds().Dy()

	yuyv := make([]byte, w*h*2)

	// YCbCr 4:2:0 → YUYV 4:2:2
	// - Y:  full resolution (w×h)
	// - Cb: subsampled horizontally and vertically by 2
	// - Cr: subsampled horizontally and vertically by 2
	for y := 0; y < h; y++ {
		for x := 0; x < w; x += 2 {
			// Y values are full resolution
			y0 := ycbcr.Y[y*ycbcr.YStride+x]
			y1 := ycbcr.Y[y*ycbcr.YStride+x+1]

			// Chroma from nearest subsampled position (each Cb/Cr sample covers 2×2 luma pixels)
			cb := ycbcr.Cb[y/2*ycbcr.CStride+x/2]
			cr := ycbcr.Cr[y/2*ycbcr.CStride+x/2]

			offset := (y*w + x) * 2
			yuyv[offset+0] = y0
			yuyv[offset+1] = cb // U
			yuyv[offset+2] = y1
			yuyv[offset+3] = cr // V
		}
	}

	return yuyv, w, h
}

func runInjectLoop(ctx context.Context, cfg *Config, dsn string,
	statsCh chan<- InjectionStats, logger *slog.Logger) InjectionStats {

	var stats InjectionStats

	pool, err := ConnectDB(ctx, dsn, logger)
	if err != nil {
		logger.Error("failed to connect DB", "err", err)
		return stats
	}
	defer pool.Close()

	injectFd, err := os.OpenFile(cfg.Kernel.InjectPath, os.O_WRONLY, 0)
	if err != nil {
		logger.Error("failed to open inject device",
			"path", cfg.Kernel.InjectPath, "err", err)
		return stats
	}
	defer injectFd.Close()

	ticker := time.NewTicker(time.Duration(cfg.Daemon.PollIntervalS) * time.Second)
	defer ticker.Stop()

	// SQL: get latest screenshot for a device from vision_captures
	const query = `SELECT file_path, resolution, capture_type
		FROM vision_captures
		WHERE device_id = $1
		ORDER BY time DESC
		LIMIT 1`

	// Track last injected file path per device to skip duplicates
	lastPath := make(map[int]string)

	for {
		select {
		case <-ctx.Done():
			return stats
		case <-ticker.C:
			t0 := time.Now()

			for i, dev := range cfg.Devices {
				var filePath, resolution, captureType string

				err := pool.QueryRow(ctx, query, dev.DeviceID).Scan(
					&filePath, &resolution, &captureType)
				if err != nil {
					stats.DBErrors++
					logger.Warn("DB query failed",
						"device_id", dev.DeviceID,
						"err", err)
					continue
				}

				// No file yet (vision-service may not have captured anything)
				if filePath == "" {
					stats.SkipNoFile++
					continue
				}

				// Skip if same file as last injection
				if filePath == lastPath[i] {
					stats.SkipDuplicate++
					continue
				}
				lastPath[i] = filePath

				// Read JPEG file from disk
				jpegData, err := os.ReadFile(filePath)
				if err != nil {
					stats.InjectErrors++
					logger.Warn("failed to read JPEG",
						"device_id", dev.DeviceID,
						"file_path", filePath,
						"err", err)
					continue
				}

				// Decode JPEG and convert to YUYV
				// Try fast path (YCbCr native) first, fall back to RGB path
				var yuyv []byte
				var w, h int

				ycbcrImg, err := jpeg.Decode(bytes.NewReader(jpegData))
				if err != nil {
					stats.InjectErrors++
					logger.Warn("JPEG decode failed",
						"device_id", dev.DeviceID,
						"file_path", filePath,
						"err", err)
					continue
				}

				if ycbcr, ok := ycbcrImg.(*image.YCbCr); ok {
					yuyv, w, h = jpegToYUYVFast(ycbcr)
				} else {
					// Non-YCbCr image (e.g., grayscale or paletted)
					yuyv, w, h, err = jpegToYUYV(jpegData)
					if err != nil {
						stats.InjectErrors++
						logger.Warn("RGB→YUYV conversion failed",
							"device_id", dev.DeviceID,
							"err", err)
						continue
					}
				}

				// Build and write injection struct
				inj := VideoInjection{
					DeviceID:    int32(i),
					Width:       int32(w),
					Height:      int32(h),
					PixelFormat: v4l2PixFmtYUYV,
					DataSize:    int32(len(yuyv)),
					Data:        yuyv,
				}

				buf := inj.Marshal()
				n, writeErr := injectFd.Write(buf)
				if writeErr != nil || n != len(buf) {
					stats.InjectErrors++
					logger.Warn("inject write failed",
						"device_id", dev.DeviceID,
						"file_path", filePath,
						"err", writeErr,
						"bytes_written", n,
						"expected", len(buf))
					continue
				}

				stats.Injections++
				logger.Debug("frame injected",
					"device_id", dev.DeviceID,
					"width", w,
					"height", h,
					"yuyv_bytes", len(yuyv),
					"capture_type", captureType,
					"resolution", resolution,
				)
			}

			stats.LastPollMS = time.Since(t0).Milliseconds()

			// Non-blocking send to health reporter
			select {
			case statsCh <- stats:
			default:
			}
		}
	}
}
