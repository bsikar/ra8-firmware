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

func TestCreateBackupSigningKeyPairStillAcceptsAProtectedParent(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "keys")
	if err := os.Mkdir(directory, 0o750); err != nil {
		t.Fatal(err)
	}
	if err := CreateBackupSigningKeyPair(filepath.Join(directory, "private.key"), filepath.Join(directory, "public.key")); err != nil {
		t.Fatalf("protected signing key parent was refused: %v", err)
	}
}
