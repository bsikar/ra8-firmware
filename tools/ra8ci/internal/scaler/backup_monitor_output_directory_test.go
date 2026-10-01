// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// monitorWithOutputDirectory holds the fixture's attestation directory at
// directoryMode. Every other input stays protected, so a refusal can only
// come from the output directory rule under test.
func monitorWithOutputDirectory(t *testing.T, directoryMode os.FileMode) BackupMonitorConfig {
	t.Helper()
	config, _ := backupMonitorFixture(t, 0o750)
	directory := filepath.Dir(config.AttestationPath)
	if err := os.Chmod(directory, directoryMode); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(directory, 0o700) })
	return config
}

func TestProtectedAttestationDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithOutputDirectory(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected attestation directory was refused: %v", err)
	}
	if _, err := os.Stat(config.AttestationPath); err != nil {
		t.Fatalf("an accepted run published nothing: %v", err)
	}
}

func TestOwnerWritableAttestationDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithOutputDirectory(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable attestation directory was refused: %v", err)
	}
}

// The rule the other four monitor directories already hold: group write is
// permission to unlink and rename the published attestation, whatever the
// file's own 0640 says.
func TestGroupWritableAttestationDirectoryIsRefused(t *testing.T) {
	config := monitorWithOutputDirectory(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable attestation directory was accepted")
	}
	if !strings.Contains(err.Error(), "attestation directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

func TestWorldWritableAttestationDirectoryIsRefused(t *testing.T) {
	config := monitorWithOutputDirectory(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable attestation directory was accepted")
	}
	if !strings.Contains(err.Error(), "attestation directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// Same call as every other directory in this file: the sticky bit buys no
// exception.
func TestStickyWorldWritableAttestationDirectoryIsRefused(t *testing.T) {
	config := monitorWithOutputDirectory(t, os.ModeSticky|0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("sticky world-writable attestation directory was accepted")
	}
}

// A missing or non-directory output path keeps the pre-existing message, so
// the two failures stay distinguishable to an operator.
func TestMissingAttestationDirectoryStillNamesTheOldRule(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	config.AttestationPath = filepath.Join(filepath.Dir(config.AttestationPath), "absent", "backup.json")
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("a missing attestation directory was accepted")
	}
	if !strings.Contains(err.Error(), "must be a protected real directory") {
		t.Fatalf("the pre-existing rule no longer names its own reason: %v", err)
	}
}

// CreateBackupSigningKeyPair draws the same line: exclusive creation refuses
// to replace an existing key, but it cannot stop an account that can write
// the directory from renaming the new key away and leaving its own pair.
func TestCreateBackupSigningKeyPairRefusesAGroupWritableParent(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "keys")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	// Chmod after the create: the create mode is masked by the process umask.
	if err := os.Chmod(directory, 0o770); err != nil {
		t.Fatal(err)
	}
	err := CreateBackupSigningKeyPair(filepath.Join(directory, "private.key"), filepath.Join(directory, "public.key"))
	if err == nil {
		t.Fatal("group-writable signing key parent was accepted")
	}
	if !strings.Contains(err.Error(), "must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
	if entries, readErr := os.ReadDir(directory); readErr != nil || len(entries) != 0 {
		t.Fatalf("a refused run left key material behind: %v, %d entries", readErr, len(entries))
	}
}

func TestCreateBackupSigningKeyPairStillAcceptsAProtectedParent(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "keys")
	if err := os.Mkdir(directory, 0o750); err != nil {
		t.Fatal(err)
	}
	if err := CreateBackupSigningKeyPair(filepath.Join(directory, "private.key"), filepath.Join(directory, "public.key")); err != nil {
		t.Fatalf("protected signing key parent was refused: %v", err)
	}
}
