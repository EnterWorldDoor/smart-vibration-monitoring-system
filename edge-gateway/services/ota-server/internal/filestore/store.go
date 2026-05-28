package filestore

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
)

type Store struct {
	basePath string
	logger   *slog.Logger
}

func NewStore(basePath string, logger *slog.Logger) *Store {
	return &Store{basePath: basePath, logger: logger}
}

func (s *Store) SaveFirmware(reader io.Reader, platform, fileName string) (filePath string, fileSize int64, sha256Hash string, err error) {
	dir := filepath.Join(s.basePath, platform)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return "", 0, "", fmt.Errorf("create firmware dir: %w", err)
	}

	fullPath := filepath.Join(dir, fileName)
	f, err := os.Create(fullPath)
	if err != nil {
		return "", 0, "", fmt.Errorf("create firmware file: %w", err)
	}
	defer f.Close()

	hasher := sha256.New()
	tee := io.TeeReader(reader, hasher)

	written, err := io.Copy(f, tee)
	if err != nil {
		os.Remove(fullPath)
		return "", 0, "", fmt.Errorf("write firmware file: %w", err)
	}

	filePath = filepath.Join(platform, fileName)
	fileSize = written
	sha256Hash = hex.EncodeToString(hasher.Sum(nil))

	s.logger.Info("firmware saved", "path", filePath, "size", fileSize, "sha256", sha256Hash)
	return filePath, fileSize, sha256Hash, nil
}

func (s *Store) GetFirmwarePath(platform, fileName string) (string, error) {
	if strings.Contains(fileName, "..") || strings.Contains(platform, "..") {
		return "", fmt.Errorf("invalid path")
	}
	if !strings.HasSuffix(fileName, ".bin") {
		return "", fmt.Errorf("invalid file extension")
	}
	return filepath.Join(s.basePath, platform, fileName), nil
}

func (s *Store) DeleteFirmware(platform, fileName string) error {
	path, err := s.GetFirmwarePath(platform, fileName)
	if err != nil {
		return err
	}
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("delete firmware: %w", err)
	}
	return nil
}

func (s *Store) ListFirmware(platform string) ([]string, error) {
	dir := filepath.Join(s.basePath, platform)
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, fmt.Errorf("list firmware: %w", err)
	}

	var files []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".bin") {
			files = append(files, e.Name())
		}
	}
	return files, nil
}
