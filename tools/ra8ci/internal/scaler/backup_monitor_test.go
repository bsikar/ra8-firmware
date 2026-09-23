// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestRefreshBackupAttestationEndToEnd(t *testing.T) {
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
	approvalID := "018d1234-5678-7abc-8def-123456789abc"
	drillPath := filepath.Join(root, "restore.json")
	restoreAt := time.Now().UTC().Add(-time.Hour).Truncate(time.Second)
	receipt := []byte(`{"approval_id":"` + approvalID + `","restore_drill_at":"` + restoreAt.Format(time.RFC3339) + `"}`)
	if err := os.WriteFile(drillPath, receipt, 0o640); err != nil {
		t.Fatal(err)
	}
	commandPath := filepath.Join(root, "pgbackrest")
	command := []byte("#!/bin/sh\nprintf '[{\"name\":\"ra8ci\",\"backup\":[{\"type\":\"full\",\"timestamp\":{\"stop\":%s}}]}]' \"$(date +%s)\"\n")
	if err := os.WriteFile(commandPath, command, 0o750); err != nil {
		t.Fatal(err)
	}
	outputDir := filepath.Join(root, "out")
	if err := os.Mkdir(outputDir, 0o750); err != nil {
		t.Fatal(err)
	}
	outputPath := filepath.Join(outputDir, "backup.json")
	config := BackupMonitorConfig{PgBackRestPath: commandPath, PrivateKeyPath: keyPath,
		RestoreDrillPath: drillPath, AttestationPath: outputPath, ApprovalID: approvalID, Stanza: "ra8ci"}
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("RefreshBackupAttestation: %v", err)
	}
	gate, err := NewSignedBackupGate(outputPath, public, approvalID, time.Hour, 48*time.Hour, 90*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	if err := gate.Check(context.Background(), approvalID); err != nil {
		t.Fatalf("signed evidence failed verification: %v", err)
	}
}

func TestRefreshBackupAttestationRejectsUntrustedRestoreReceipt(t *testing.T) {
	root := t.TempDir()
	receipt := filepath.Join(root, "restore.json")
	if err := os.WriteFile(receipt, []byte(`{"approval_id":"other","restore_drill_at":"2026-09-23T00:00:00Z"}`), 0o640); err != nil {
		t.Fatal(err)
	}
	config := BackupMonitorConfig{PgBackRestPath: filepath.Join(root, "pgbackrest"),
		PrivateKeyPath: filepath.Join(root, "key"), RestoreDrillPath: receipt,
		AttestationPath: filepath.Join(root, "backup.json"),
		ApprovalID:      "018d1234-5678-7abc-8def-123456789abc", Stanza: "ra8ci"}
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("foreign restore receipt was accepted")
	}
}
