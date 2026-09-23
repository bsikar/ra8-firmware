// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package boardagent owns durable board-agent generation fencing. It never
// stores signing keys or permits task code to edit the generation record.
package boardagent

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
)

var (
	ErrUnsafeState        = errors.New("unsafe board-agent state file")
	ErrGenerationInvalid  = errors.New("invalid board-agent generation")
	ErrGenerationRollback = errors.New("board-agent generation cannot move backward")
)

// HighWaterStore persists the highest installed server generation.
type HighWaterStore interface {
	Load() (uint64, error)
	Advance(uint64) error
}

// FileHighWater atomically stores one board's monotonic generation in a
// protected, single-writer state file beneath the board-agent service account.
type FileHighWater struct {
	path    string
	boardID string
	mu      sync.Mutex
}

// NewFileHighWater binds a protected state file to one board identity.
func NewFileHighWater(path, boardID string) (*FileHighWater, error) {
	if !validBoardID(boardID) || path == "" {
		return nil, ErrUnsafeState
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		return nil, ErrUnsafeState
	}
	directory := filepath.Dir(absolute)
	resolved, err := filepath.EvalSymlinks(directory)
	if err != nil || filepath.Clean(resolved) != filepath.Clean(directory) {
		return nil, ErrUnsafeState
	}
	dirInfo, err := os.Lstat(directory)
	if err != nil || !dirInfo.IsDir() || dirInfo.Mode()&os.ModeSymlink != 0 ||
		dirInfo.Mode().Perm()&0o077 != 0 {
		return nil, ErrUnsafeState
	}
	if fileInfo, err := os.Lstat(absolute); err == nil {
		if !fileInfo.Mode().IsRegular() || fileInfo.Mode()&os.ModeSymlink != 0 ||
			fileInfo.Mode().Perm()&0o077 != 0 {
			return nil, ErrUnsafeState
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, ErrUnsafeState
	}
	return &FileHighWater{path: absolute, boardID: boardID}, nil
}

// Load returns zero for an uninitialized store and otherwise validates the
// exact board binding, file identity, permissions, schema, and canonical form.
func (s *FileHighWater) Load() (uint64, error) {
	if s == nil {
		return 0, ErrUnsafeState
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.loadLocked()
}

func (s *FileHighWater) loadLocked() (uint64, error) {
	info, err := os.Lstat(s.path)
	if errors.Is(err, os.ErrNotExist) {
		return 0, nil
	}
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 ||
		info.Size() < 1 || info.Size() > 512 || info.Mode().Perm()&0o077 != 0 {
		return 0, ErrUnsafeState
	}
	file, err := os.Open(s.path)
	if err != nil {
		return 0, ErrUnsafeState
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) ||
		opened.Size() < 1 || opened.Size() > 512 || opened.Mode().Perm()&0o077 != 0 {
		return 0, ErrUnsafeState
	}
	raw, err := io.ReadAll(io.LimitReader(file, 513))
	if err != nil || len(raw) > 512 {
		return 0, ErrUnsafeState
	}
	fields := make(map[string]string, 3)
	for _, line := range strings.Split(strings.TrimSuffix(string(raw), "\n"), "\n") {
		key, value, found := strings.Cut(line, "=")
		if !found || key == "" || value == "" {
			return 0, ErrUnsafeState
		}
		if _, duplicate := fields[key]; duplicate {
			return 0, ErrUnsafeState
		}
		fields[key] = value
	}
	if len(fields) != 3 || fields["schema_version"] != "1" || fields["board_id"] != s.boardID {
		return 0, ErrUnsafeState
	}
	highWater, err := strconv.ParseUint(fields["high_water"], 10, 64)
	if err != nil || strconv.FormatUint(highWater, 10) != fields["high_water"] {
		return 0, ErrUnsafeState
	}
	return highWater, nil
}

// Advance durably records a higher generation before the caller acknowledges
// that grant to the server. Equal writes are idempotent; lower values fail.
func (s *FileHighWater) Advance(generation uint64) error {
	if s == nil || generation == 0 {
		return ErrGenerationInvalid
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	current, err := s.loadLocked()
	if err != nil {
		return err
	}
	if generation < current {
		return ErrGenerationRollback
	}
	if generation == current {
		return nil
	}
	data := fmt.Sprintf("schema_version=1\nboard_id=%s\nhigh_water=%d\n", s.boardID, generation)
	directory := filepath.Dir(s.path)
	temporary, err := os.CreateTemp(directory, ".ra8ci-board-state-*.tmp")
	if err != nil {
		return ErrUnsafeState
	}
	tempPath := temporary.Name()
	defer func() { _ = os.Remove(tempPath) }()
	if err := temporary.Chmod(0o600); err != nil {
		_ = temporary.Close()
		return ErrUnsafeState
	}
	if _, err := temporary.WriteString(data); err != nil {
		_ = temporary.Close()
		return ErrUnsafeState
	}
	if err := temporary.Sync(); err != nil {
		_ = temporary.Close()
		return ErrUnsafeState
	}
	if err := temporary.Close(); err != nil {
		return ErrUnsafeState
	}
	if err := os.Rename(tempPath, s.path); err != nil {
		return ErrUnsafeState
	}
	dir, err := os.Open(directory)
	if err != nil {
		return ErrUnsafeState
	}
	defer dir.Close()
	if err := dir.Sync(); err != nil {
		return ErrUnsafeState
	}
	return nil
}

func validBoardID(value string) bool {
	if value == "" || len(value) > 128 || strings.TrimSpace(value) != value {
		return false
	}
	for _, character := range value {
		if !((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || strings.ContainsRune("-_.", character)) {
			return false
		}
	}
	return true
}
