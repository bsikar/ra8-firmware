// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// BackupMonitorConfig is supplied only to a separately privileged monitor
// process. Its signing key is never available to the ra8ci API service.
type BackupMonitorConfig struct {
	PgBackRestPath   string
	PrivateKeyPath   string
	RestoreDrillPath string
	AttestationPath  string
	ApprovalID       string
	Stanza           string
}

type restoreDrillReceipt struct {
	ApprovalID     string    `json:"approval_id"`
	RestoreDrillAt time.Time `json:"restore_drill_at"`
}

// RefreshBackupAttestation queries pgBackRest and signs a bounded status
// record. Callers should run this as the dedicated backup-monitor identity.
// Every file it reads or executes must be a bounded regular file that no
// other account can write, the pgBackRest executable included. A directory
// holding one of those files is held to the same rule, since an account that
// can write the directory can replace the file inside it whatever the file's
// own permissions say.
func RefreshBackupAttestation(ctx context.Context, config BackupMonitorConfig) error {
	if ctx == nil || !filepath.IsAbs(config.PgBackRestPath) || !filepath.IsAbs(config.PrivateKeyPath) ||
		!filepath.IsAbs(config.RestoreDrillPath) || !filepath.IsAbs(config.AttestationPath) ||
		!validBackupStanza(config.Stanza) ||
		!store.ValidID(config.ApprovalID) {
		return errors.New("invalid backup monitor configuration")
	}
	drillDirInfo, err := os.Lstat(filepath.Dir(config.RestoreDrillPath))
	if err != nil || !drillDirInfo.IsDir() || drillDirInfo.Mode()&os.ModeSymlink != 0 {
		return errors.New("restore drill receipt directory must be a real directory")
	}
	if drillDirInfo.Mode().Perm()&0022 != 0 {
		return errors.New("restore drill receipt directory must not be group or world writable")
	}
	drillInfo, err := os.Lstat(config.RestoreDrillPath)
	if err != nil || !drillInfo.Mode().IsRegular() || drillInfo.Mode().Perm()&0022 != 0 || drillInfo.Size() <= 0 || drillInfo.Size() > 4096 {
		return errors.New("restore drill receipt must be a bounded, protected regular file")
	}
	drillFile, err := os.Open(config.RestoreDrillPath)
	if err != nil {
		return errors.New("open restore drill receipt")
	}
	defer drillFile.Close()
	openedDrill, err := drillFile.Stat()
	if err != nil || !os.SameFile(drillInfo, openedDrill) {
		return errors.New("restore drill receipt changed while opening")
	}
	drillRaw, err := io.ReadAll(io.LimitReader(drillFile, 4097))
	if err != nil || len(drillRaw) > 4096 {
		return errors.New("restore drill receipt exceeds size limit")
	}
	var drill restoreDrillReceipt
	dec := json.NewDecoder(strings.NewReader(string(drillRaw)))
	dec.DisallowUnknownFields()
	if dec.Decode(&drill) != nil || drill.ApprovalID != config.ApprovalID || drill.RestoreDrillAt.IsZero() {
		return errors.New("restore drill receipt is invalid or belongs to another approval")
	}
	var trailing any
	if dec.Decode(&trailing) != io.EOF {
		return errors.New("restore drill receipt has trailing JSON")
	}
	keyInfo, err := os.Lstat(config.PrivateKeyPath)
	if err != nil || !keyInfo.Mode().IsRegular() || keyInfo.Mode().Perm()&0077 != 0 || keyInfo.Size() > 256 {
		return errors.New("backup signing key must be a bounded private regular file")
	}
	keyFile, err := os.Open(config.PrivateKeyPath)
	if err != nil {
		return errors.New("open backup signing key")
	}
	defer keyFile.Close()
	openedKey, err := keyFile.Stat()
	if err != nil || !os.SameFile(keyInfo, openedKey) {
		return errors.New("backup signing key changed while opening")
	}
	keyRaw, err := io.ReadAll(io.LimitReader(keyFile, 257))
	if err != nil || len(keyRaw) > 256 {
		return errors.New("backup signing key exceeds size limit")
	}
	keyBytes, err := base64.StdEncoding.Strict().DecodeString(strings.TrimSpace(string(keyRaw)))
	clear(keyRaw)
	if err != nil || len(keyBytes) != ed25519.PrivateKeySize {
		clear(keyBytes)
		return errors.New("backup signing key is not a base64 Ed25519 private key")
	}
	defer clear(keyBytes)
	binaryInfo, err := os.Lstat(config.PgBackRestPath)
	if err != nil || !binaryInfo.Mode().IsRegular() || binaryInfo.Mode()&os.ModeSymlink != 0 || binaryInfo.Mode().Perm()&0111 == 0 {
		return errors.New("pgBackRest executable must be an executable regular file")
	}
	if binaryInfo.Mode().Perm()&0022 != 0 {
		return errors.New("pgBackRest executable must not be group or world writable")
	}
	commandDirInfo, err := os.Lstat(filepath.Dir(config.PgBackRestPath))
	if err != nil || !commandDirInfo.IsDir() || commandDirInfo.Mode()&os.ModeSymlink != 0 {
		return errors.New("pgBackRest executable directory must be a real directory")
	}
	if commandDirInfo.Mode().Perm()&0022 != 0 {
		return errors.New("pgBackRest executable directory must not be group or world writable")
	}
	commandCtx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	command := exec.CommandContext(commandCtx, config.PgBackRestPath, "--output=json", "--stanza="+config.Stanza, "info")
	command.Env = []string{"PATH=/usr/bin:/bin", "HOME=/var/lib/postgresql", "LANG=C", "LC_ALL=C"}
	var stdout, stderr boundedBackupBuffer
	stdout.limit, stderr.limit = 8<<20, 16<<10
	command.Stdout, command.Stderr = &stdout, &stderr
	if err := command.Run(); err != nil {
		clear(stdout.data)
		clear(stderr.data)
		return errors.New("pgBackRest info command failed")
	}
	latestFull, err := ParseLatestFullBackupInfo(stdout.data, config.Stanza)
	clear(stdout.data)
	clear(stderr.data)
	if err != nil {
		return err
	}
	now := time.Now().UTC()
	attestation := BackupAttestation{SchemaVersion: backupAttestationSchema, ApprovalID: config.ApprovalID,
		CheckedAt: now, LatestFullBackup: latestFull, RestoreDrillAt: drill.RestoreDrillAt.UTC()}
	encoded, err := SignBackupAttestation(attestation, ed25519.PrivateKey(keyBytes))
	if err != nil {
		return err
	}
	if err := writeBackupAttestation(config.AttestationPath, encoded); err != nil {
		return err
	}
	return nil
}

type boundedBackupBuffer struct {
	data  []byte
	limit int
}

func (b *boundedBackupBuffer) Write(p []byte) (int, error) {
	if len(p) > b.limit-len(b.data) {
		return 0, errors.New("backup monitor output exceeds limit")
	}
	b.data = append(b.data, p...)
	return len(p), nil
}

func writeBackupAttestation(path string, data []byte) error {
	directory := filepath.Dir(path)
	info, err := os.Lstat(directory)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm()&0002 != 0 {
		return errors.New("backup attestation directory must be a protected real directory")
	}
	temporary, err := os.CreateTemp(directory, ".ra8ci-backup-attestation-*")
	if err != nil {
		return errors.New("create temporary backup attestation")
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if err := temporary.Chmod(0o640); err != nil {
		_ = temporary.Close()
		return errors.New("set backup attestation permissions")
	}
	if _, err := temporary.Write(data); err != nil {
		_ = temporary.Close()
		return errors.New("write backup attestation")
	}
	if err := temporary.Sync(); err != nil {
		_ = temporary.Close()
		return errors.New("sync backup attestation")
	}
	if err := temporary.Close(); err != nil {
		return errors.New("close backup attestation")
	}
	if err := os.Rename(temporaryPath, path); err != nil {
		return fmt.Errorf("publish backup attestation: %w", err)
	}
	directoryFile, err := os.Open(directory)
	if err != nil {
		return errors.New("open backup attestation directory")
	}
	defer directoryFile.Close()
	if err := directoryFile.Sync(); err != nil {
		return errors.New("sync backup attestation directory")
	}
	return nil
}

// LoadBackupPublicKey reads a bounded base64 Ed25519 public key from a
// non-writable regular file available to the API service.
func LoadBackupPublicKey(path string) (ed25519.PublicKey, error) {
	if !filepath.IsAbs(path) {
		return nil, errors.New("backup public key path must be absolute")
	}
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm()&0022 != 0 || info.Size() <= 0 || info.Size() > 256 {
		return nil, errors.New("backup public key must be a bounded non-writable regular file")
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, errors.New("open backup public key")
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !os.SameFile(info, opened) {
		return nil, errors.New("backup public key changed while opening")
	}
	raw, err := io.ReadAll(io.LimitReader(file, 257))
	if err != nil || len(raw) > 256 {
		return nil, errors.New("backup public key exceeds size limit")
	}
	decoded, err := base64.StdEncoding.Strict().DecodeString(strings.TrimSpace(string(raw)))
	clear(raw)
	if err != nil || len(decoded) != ed25519.PublicKeySize {
		clear(decoded)
		return nil, errors.New("backup public key is malformed")
	}
	return ed25519.PublicKey(decoded), nil
}
