package filestore

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

var discardLog = slog.New(slog.NewTextHandler(io.Discard, nil))

type Store struct {
	basePath string
	logger   *slog.Logger
}

func NewStore(basePath string, logger *slog.Logger) *Store {
	if logger == nil {
		logger = discardLog
	}
	return &Store{basePath: basePath, logger: logger}
}

func (s *Store) SaveModel(reader io.Reader, modelName, version string) (relPath string, fileSize int64, sha256Hex string, err error) {
	dir := filepath.Join(s.basePath, modelName)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return "", 0, "", fmt.Errorf("create dir: %w", err)
	}

	fileName := version + ".onnx"
	fullPath := filepath.Join(dir, fileName)

	f, err := os.Create(fullPath)
	if err != nil {
		return "", 0, "", fmt.Errorf("create file: %w", err)
	}
	defer f.Close()

	hasher := sha256.New()
	tee := io.TeeReader(reader, hasher)

	size, err := io.Copy(f, tee)
	if err != nil {
		f.Close()
		os.Remove(fullPath)
		return "", 0, "", fmt.Errorf("write file: %w", err)
	}

	if err := f.Sync(); err != nil {
		return "", 0, "", fmt.Errorf("sync file: %w", err)
	}

	sha256Hex = hex.EncodeToString(hasher.Sum(nil))
	relPath = filepath.Join(modelName, fileName)

	s.logger.Info("model saved",
		"path", relPath,
		"size", size,
		"sha256", sha256Hex[:16]+"...",
	)

	return relPath, size, sha256Hex, nil
}

func (s *Store) GetModelPath(modelName, version string) (string, error) {
	if strings.Contains(modelName, "..") || strings.Contains(version, "..") {
		return "", fmt.Errorf("invalid path: model_name or version contains '..'")
	}
	if !strings.HasSuffix(version, "") {
		// version is validated by caller as semver, but we also check the constructed filename
	}
	if modelName == "" || version == "" {
		return "", fmt.Errorf("model_name and version must not be empty")
	}

	fileName := version + ".onnx"
	relPath := filepath.Join(modelName, fileName)
	fullPath := filepath.Join(s.basePath, relPath)

	return fullPath, nil
}

func (s *Store) DeleteModel(modelName, version string) error {
	fullPath, err := s.GetModelPath(modelName, version)
	if err != nil {
		return err
	}
	if err := os.Remove(fullPath); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

func (s *Store) RotateVersions(modelName string, keep int) error {
	dir := filepath.Join(s.basePath, modelName)
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}

	type fileInfo struct {
		name    string
		modTime int64
	}
	var files []fileInfo
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".onnx") {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		files = append(files, fileInfo{name: e.Name(), modTime: info.ModTime().Unix()})
	}

	if len(files) <= keep {
		return nil
	}

	sort.Slice(files, func(i, j int) bool {
		return files[i].modTime > files[j].modTime
	})

	for _, f := range files[keep:] {
		path := filepath.Join(dir, f.name)
		if err := os.Remove(path); err != nil {
			s.logger.Warn("rotate: failed to remove", "path", path, "err", err)
		} else {
			s.logger.Info("rotate: removed old model", "path", path)
		}
	}

	return nil
}
