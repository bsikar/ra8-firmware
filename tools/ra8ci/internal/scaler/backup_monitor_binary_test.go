// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const monitorApprovalID = "018d1234-5678-7abc-8def-123456789abc"

// backupMonitorFixture writes a complete, valid monitor input set and returns
// the config plus the verifying key. Only the pgBackRest executable's
// permissions vary, so a refusal can only come from the rule under test.
func backupMonitorFixture(t *testing.T, binaryMode os.FileMode) (BackupMonitorConfig, ed25519.PublicKey) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	keyPath := filepath.Join(root, "signing.key")
	key := []byte(base64.StdEncoding.EncodeToString(private))
	if err := os.WriteFile(keyPath, key, 0o600); err != nil {
		t.Fatal(err)
	}
	clear(key)
	drillPath := filepath.Join(root, "restore.json")
	restoreAt := time.Now().UTC().Add(-time.Hour).Truncate(time.Second)
	receipt := []byte(`{"approval_id":"` + monitorApprovalID + `","restore_drill_at":"` + restoreAt.Format(time.RFC3339) + `"}`)
	if err := os.WriteFile(drillPath, receipt, 0o640); err != nil {
		t.Fatal(err)
	}
	commandPath := filepath.Join(root, "pgbackrest")
	command := []byte("#!/bin/sh\nprintf '[{\"name\":\"ra8ci\",\"backup\":[{\"type\":\"full\",\"timestamp\":{\"stop\":%s}}]}]' \"$(date +%s)\"\n")
	if err := os.WriteFile(commandPath, command, 0o600); err != nil {
		t.Fatal(err)
	}
	// Chmod explicitly: the create mode is masked by the process umask, which
	// would quietly strip the very bits these tests are about.
	if err := os.Chmod(commandPath, binaryMode); err != nil {
		t.Fatal(err)
	}
	outputDir := filepath.Join(root, "out")
	if err := os.Mkdir(outputDir, 0o750); err != nil {
		t.Fatal(err)
	}
	config := BackupMonitorConfig{PgBackRestPath: commandPath, PrivateKeyPath: keyPath,
		RestoreDrillPath: drillPath, AttestationPath: filepath.Join(outputDir, "backup.json"),
		ApprovalID: monitorApprovalID, Stanza: "ra8ci"}
	return config, public
}

func TestProtectedPgBackRestExecutableIsStillAccepted(t *testing.T) {
	config, public := backupMonitorFixture(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected executable was refused: %v", err)
	}
	gate, err := NewSignedBackupGate(config.AttestationPath, public, config.ApprovalID, time.Hour, 48*time.Hour, 90*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	if err := gate.Check(context.Background(), config.ApprovalID); err != nil {
		t.Fatalf("signed evidence failed verification: %v", err)
	}
}

func TestOwnerWritablePgBackRestExecutableIsStillAccepted(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable executable was refused: %v", err)
	}
}

func TestGroupWritablePgBackRestExecutableIsRefused(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable pgBackRest executable was accepted")
	}
	if !strings.Contains(err.Error(), "group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritablePgBackRestExecutableIsRefused(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable pgBackRest executable was accepted")
	}
	if !strings.Contains(err.Error(), "group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWritablePgBackRestExecutableSignsNothing(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("world-writable pgBackRest executable was accepted")
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

func TestNonExecutablePgBackRestIsStillRefusedAsNotExecutable(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o640)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("non-executable pgBackRest was accepted")
	}
	if !strings.Contains(err.Error(), "executable regular file") {
		t.Fatalf("the pre-existing rule no longer names its own reason: %v", err)
	}
}
