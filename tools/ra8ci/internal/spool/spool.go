// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package spool durably records local task attempts while the server is absent.
package spool

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

const schemaVersion = 2

// SourceIdentity records what the local checkout could actually prove before
// execution. Unverified means the working tree or submodules were not clean;
// it is never upgraded to trusted CI evidence during upload.
type SourceIdentity struct {
	Repository     string `json:"repository"`
	Branch         string `json:"branch"`
	CommitSHA      string `json:"commit_sha"`
	SnapshotSHA256 string `json:"snapshot_sha256,omitempty"`
	Verification   string `json:"verification"`
}

// Metadata freezes reviewed task and source identity before the first command.
type Metadata struct {
	Source          SourceIdentity `json:"source"`
	Tier            string         `json:"tier"`
	Scope           string         `json:"scope"`
	DeadlineSeconds int            `json:"deadline_seconds"`
	Args            []string       `json:"args"`
}

// Entry contains only task metadata and log hashes, never stdout or secrets.
type Entry struct {
	SchemaVersion   int              `json:"schema_version"`
	ID              string           `json:"id"`
	Task            string           `json:"task"`
	CatalogDigest   string           `json:"catalog_digest"`
	Source          SourceIdentity   `json:"source"`
	Tier            string           `json:"tier"`
	Scope           string           `json:"scope"`
	DeadlineSeconds int              `json:"deadline_seconds"`
	Args            []string         `json:"args"`
	StartedAt       time.Time        `json:"started_at"`
	FinishedAt      *time.Time       `json:"finished_at,omitempty"`
	Result          *executor.Result `json:"result,omitempty"`
	Error           string           `json:"error,omitempty"`
	SyncState       string           `json:"sync_state"`
}

// Spool is a private append-only local outbox.
type Spool struct{ directory string }

// DefaultDirectory chooses a per-user state directory, not a checkout path.
func DefaultDirectory() (string, error) {
	return defaultDirectory(runtime.GOOS, os.Getenv, os.UserHomeDir)
}

func defaultDirectory(goos string, getenv func(string) string, userHome func() (string, error)) (string, error) {
	if custom := getenv("RA8CI_STATE_DIR"); custom != "" {
		if !filepath.IsAbs(custom) {
			return "", errors.New("RA8CI_STATE_DIR must be absolute")
		}
		return custom, nil
	}
	if goos == "windows" {
		base := getenv("LOCALAPPDATA")
		if base == "" {
			return "", errors.New("LOCALAPPDATA is required for offline state")
		}
		return filepath.Join(base, "ra8ci", "outbox"), nil
	}
	base := getenv("XDG_STATE_HOME")
	if base == "" {
		home, err := userHome()
		if err != nil {
			return "", err
		}
		base = filepath.Join(home, ".local", "state")
	}
	if !filepath.IsAbs(base) {
		return "", errors.New("state base must be absolute")
	}
	return filepath.Join(base, "ra8ci", "outbox"), nil
}

// Open creates a private spool and rejects a symlink or shared directory.
func Open(directory string) (*Spool, error) {
	if !filepath.IsAbs(directory) {
		return nil, errors.New("spool directory must be absolute")
	}
	if err := os.MkdirAll(directory, 0700); err != nil {
		return nil, err
	}
	info, err := os.Lstat(directory)
	if err != nil {
		return nil, err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return nil, errors.New("spool path is not a real directory")
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0077 != 0 {
		return nil, errors.New("spool directory is accessible to other users")
	}
	return &Spool{directory: directory}, nil
}

// Begin persists the existence of an attempt before any task command starts.
func (s *Spool) Begin(task, digest string) (Entry, error) {
	return s.begin(task, digest, Metadata{})
}

// BeginWithMetadata persists verified-or-explicitly-unverified source and the
// exact reviewed task declaration before execution begins.
func (s *Spool) BeginWithMetadata(task, digest string, metadata Metadata) (Entry, error) {
	if metadata.Source.Repository == "" ||
		!hexDigest(metadata.Source.CommitSHA, 40) ||
		(metadata.Source.Verification != "verified" && metadata.Source.Verification != "unverified") ||
		(metadata.Source.Verification == "verified" && !hexDigest(metadata.Source.SnapshotSHA256, 64)) ||
		(metadata.Source.Verification == "unverified" && metadata.Source.SnapshotSHA256 != "") ||
		metadata.Tier == "" || metadata.Scope == "" || metadata.DeadlineSeconds < 1 ||
		metadata.DeadlineSeconds > 86400 {
		return Entry{}, errors.New("invalid local source or task metadata")
	}
	return s.begin(task, digest, metadata)
}

func (s *Spool) begin(task, digest string, metadata Metadata) (Entry, error) {
	if s == nil || task == "" || !hexDigest(digest, 64) {
		return Entry{}, errors.New("invalid local run identity")
	}
	var entropy [16]byte
	if _, err := rand.Read(entropy[:]); err != nil {
		return Entry{}, err
	}
	version := schemaVersion
	if metadata.Source.Repository == "" {
		version = 1
	}
	entry := Entry{SchemaVersion: version, ID: hex.EncodeToString(entropy[:]), Task: task,
		CatalogDigest: digest, Source: metadata.Source, Tier: metadata.Tier,
		Scope: metadata.Scope, DeadlineSeconds: metadata.DeadlineSeconds,
		Args: append([]string(nil), metadata.Args...), StartedAt: time.Now().UTC(), SyncState: "running"}
	if err := s.write(entry.ID+".started.json", entry); err != nil {
		return Entry{}, err
	}
	return entry, nil
}

// Finish appends an unsynced terminal record without erasing the start record.
func (s *Spool) Finish(entry Entry, result executor.Result, runErr error) (Entry, error) {
	if s == nil || !validID(entry.ID) || entry.SyncState != "running" {
		return Entry{}, errors.New("invalid running local record")
	}
	if _, err := os.Lstat(filepath.Join(s.directory, entry.ID+".started.json")); err != nil {
		return Entry{}, fmt.Errorf("missing start record: %w", err)
	}
	now := time.Now().UTC()
	entry.FinishedAt = &now
	entry.Result = &result
	entry.SyncState = "unsynced"
	if runErr != nil {
		entry.Error = runErr.Error()
	}
	if err := s.write(entry.ID+".finished.json", entry); err != nil {
		return Entry{}, err
	}
	return entry, nil
}

// Pending returns terminal records that have not been acknowledged by server.
func (s *Spool) Pending() ([]Entry, error) {
	if s == nil {
		return nil, errors.New("nil spool")
	}
	files, err := os.ReadDir(s.directory)
	if err != nil {
		return nil, err
	}
	var pending []Entry
	for _, file := range files {
		if !strings.HasSuffix(file.Name(), ".finished.json") || file.IsDir() {
			continue
		}
		id := strings.TrimSuffix(file.Name(), ".finished.json")
		if !validID(id) {
			return nil, fmt.Errorf("invalid spool entry %q", file.Name())
		}
		if _, err := os.Stat(filepath.Join(s.directory, id+".synced.json")); err == nil {
			continue
		} else if !errors.Is(err, os.ErrNotExist) {
			return nil, err
		}
		raw, err := os.ReadFile(filepath.Join(s.directory, file.Name()))
		if err != nil {
			return nil, err
		}
		var entry Entry
		if err := json.Unmarshal(raw, &entry); err != nil {
			return nil, err
		}
		if entry.ID != id || entry.SyncState != "unsynced" || entry.FinishedAt == nil {
			return nil, fmt.Errorf("invalid terminal record %q", file.Name())
		}
		pending = append(pending, entry)
	}
	return pending, nil
}

// MarkSynced appends a receipt only after the server durably confirms ingest.
func (s *Spool) MarkSynced(id, serverRunID string) error {
	if s == nil || !validID(id) || serverRunID == "" {
		return errors.New("invalid sync receipt")
	}
	if _, err := os.Stat(filepath.Join(s.directory, id+".finished.json")); err != nil {
		return err
	}
	return s.write(id+".synced.json", map[string]string{"local_id": id, "server_run_id": serverRunID})
}

func (s *Spool) write(name string, value any) error {
	if strings.ContainsAny(name, `/\\`) {
		return errors.New("invalid spool name")
	}
	file, err := os.CreateTemp(s.directory, ".ra8ci-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if err := file.Chmod(0600); err != nil {
		file.Close()
		return err
	}
	enc := json.NewEncoder(file)
	if err := enc.Encode(value); err != nil {
		file.Close()
		return err
	}
	if err := file.Sync(); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	if err := os.Link(file.Name(), filepath.Join(s.directory, name)); err != nil {
		return err
	}
	if runtime.GOOS != "windows" {
		dir, err := os.Open(s.directory)
		if err != nil {
			return err
		}
		defer dir.Close()
		if err := dir.Sync(); err != nil {
			return err
		}
	}
	return nil
}

func validID(id string) bool {
	if len(id) != 32 {
		return false
	}
	for _, c := range id {
		if !strings.ContainsRune("0123456789abcdef", c) {
			return false
		}
	}
	return true
}

func hexDigest(value string, length int) bool {
	if len(value) != length {
		return false
	}
	for _, char := range value {
		if !strings.ContainsRune("0123456789abcdef", char) {
			return false
		}
	}
	return true
}
