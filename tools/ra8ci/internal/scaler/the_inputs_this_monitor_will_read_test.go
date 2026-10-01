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

// The directory tests beside this file judge where the monitor's inputs live.
// This one judges the inputs themselves: the shape of the signing key and of
// the restore drill receipt. Both are read before a single byte is signed, so
// each refusal is asserted to have published nothing.

func monitorRefusal(t *testing.T, config BackupMonitorConfig, want string) {
	t.Helper()
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("an unsound monitor input was accepted")
	}
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
	if _, statErr := os.Stat(config.AttestationPath); !os.IsNotExist(statErr) {
		t.Fatal("a refused run published an attestation")
	}
}

// writeDrillReceipt replaces the fixture's receipt with raw, keeping the 0640
// the receipt rule expects so a refusal can only come from the content.
func writeDrillReceipt(t *testing.T, config BackupMonitorConfig, raw string) {
	t.Helper()
	if err := os.WriteFile(config.RestoreDrillPath, []byte(raw), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(config.RestoreDrillPath, 0o640); err != nil {
		t.Fatal(err)
	}
}

func TestASigningKeyThatIsNotThereNamesTheKey(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := os.Remove(config.PrivateKeyPath); err != nil {
		t.Fatal(err)
	}
	monitorRefusal(t, config, "backup signing key must be a bounded private regular file")
}

func TestASigningKeyThatIsADirectoryNamesTheKey(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := os.Remove(config.PrivateKeyPath); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(config.PrivateKeyPath, 0o700); err != nil {
		t.Fatal(err)
	}
	monitorRefusal(t, config, "backup signing key must be a bounded private regular file")
}

// A base64 Ed25519 private key is 88 bytes, so the 256-byte bound is not the
// key's own length: it is there to stop the monitor reading a file that only
// happens to sit at the key's path.
func TestASigningKeyOverTheSizeBoundNamesTheKey(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := os.WriteFile(config.PrivateKeyPath, []byte(strings.Repeat("A", 257)), 0o600); err != nil {
		t.Fatal(err)
	}
	monitorRefusal(t, config, "backup signing key must be a bounded private regular file")
}

func TestASigningKeyAtTheSizeBoundIsStillReadAndThenJudgedOnItsContent(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := os.WriteFile(config.PrivateKeyPath, []byte(strings.Repeat("A", 256)), 0o600); err != nil {
		t.Fatal(err)
	}
	// Exactly at the bound the file is read, so the refusal has to come from
	// the key material rather than the size rule.
	monitorRefusal(t, config, "backup signing key is not a base64 Ed25519 private key")
}

func TestASigningKeyThatIsNotBase64NamesTheKeyMaterial(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := os.WriteFile(config.PrivateKeyPath, []byte("not base64 at all !!!"), 0o600); err != nil {
		t.Fatal(err)
	}
	monitorRefusal(t, config, "backup signing key is not a base64 Ed25519 private key")
}

// Sound base64 of the wrong length is the near miss worth pinning: a public
// key sitting at the private key's path decodes cleanly and is still refused.
func TestAPublicKeyAtThePrivateKeyPathIsRefusedOnItsLength(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	public, _, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(config.PrivateKeyPath, []byte(base64.StdEncoding.EncodeToString(public)), 0o600); err != nil {
		t.Fatal(err)
	}
	monitorRefusal(t, config, "backup signing key is not a base64 Ed25519 private key")
}

// The key is read as base64 after a trim, so the newline a text editor leaves
// behind must not turn a sound key into a refusal.
func TestASigningKeyWithATrailingNewlineIsStillAccepted(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	raw, err := os.ReadFile(config.PrivateKeyPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(config.PrivateKeyPath, append(raw, '\n'), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("a key with a trailing newline was refused: %v", err)
	}
}

func TestADrillReceiptThatIsNotThereNamesTheReceipt(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := os.Remove(config.RestoreDrillPath); err != nil {
		t.Fatal(err)
	}
	monitorRefusal(t, config, "restore drill receipt must be a bounded, protected regular file")
}

// An empty receipt is refused on its size rather than read as JSON: a
// truncated write is the likely way one appears, and the size rule says so.
func TestAnEmptyDrillReceiptNamesTheReceipt(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	writeDrillReceipt(t, config, "")
	monitorRefusal(t, config, "restore drill receipt must be a bounded, protected regular file")
}

func TestADrillReceiptOverTheSizeBoundNamesTheReceipt(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	padded := `{"approval_id":"` + monitorApprovalID + `","restore_drill_at":"` +
		time.Now().UTC().Add(-time.Hour).Format(time.RFC3339) + `"}` + strings.Repeat(" ", 4096)
	writeDrillReceipt(t, config, padded)
	monitorRefusal(t, config, "restore drill receipt must be a bounded, protected regular file")
}

func TestADrillReceiptThatIsADirectoryNamesTheReceipt(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := os.Remove(config.RestoreDrillPath); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(config.RestoreDrillPath, 0o750); err != nil {
		t.Fatal(err)
	}
	monitorRefusal(t, config, "restore drill receipt must be a bounded, protected regular file")
}

// A second document after the receipt is how an appended record hides behind
// a sound first one, so it is refused by name rather than quietly ignored.
func TestADrillReceiptWithASecondDocumentIsRefusedForTheTrailingJSON(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	stamp := time.Now().UTC().Add(-time.Hour).Format(time.RFC3339)
	writeDrillReceipt(t, config, `{"approval_id":"`+monitorApprovalID+`","restore_drill_at":"`+stamp+`"}`+
		`{"approval_id":"`+monitorApprovalID+`","restore_drill_at":"`+stamp+`"}`)
	monitorRefusal(t, config, "restore drill receipt has trailing JSON")
}

func TestADrillReceiptCarryingAnUnknownFieldIsRefused(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	stamp := time.Now().UTC().Add(-time.Hour).Format(time.RFC3339)
	writeDrillReceipt(t, config, `{"approval_id":"`+monitorApprovalID+`","restore_drill_at":"`+stamp+
		`","drill_operator":"someone"}`)
	monitorRefusal(t, config, "restore drill receipt is invalid or belongs to another approval")
}

// A receipt with no drill time at all is the one shape that would otherwise
// sign an attestation claiming a drill that never happened.
func TestADrillReceiptWithNoDrillTimeIsRefused(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	writeDrillReceipt(t, config, `{"approval_id":"`+monitorApprovalID+`"}`)
	monitorRefusal(t, config, "restore drill receipt is invalid or belongs to another approval")
}

func TestADrillReceiptThatIsNotJSONIsRefused(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	writeDrillReceipt(t, config, "the drill went fine, trust me\n")
	monitorRefusal(t, config, "restore drill receipt is invalid or belongs to another approval")
}

// The drill time is carried into the signed envelope as UTC, so a receipt
// stamped in another zone must be published as the same instant rather than
// the same wall clock.
func TestADrillTimeInAnotherZoneIsSignedAsTheSameInstant(t *testing.T) {
	config, public := backupMonitorFixture(t, 0o750)
	zone := time.FixedZone("UTC+7", 7*3600)
	drilled := time.Now().Add(-2 * time.Hour).Truncate(time.Second).In(zone)
	writeDrillReceipt(t, config, `{"approval_id":"`+monitorApprovalID+`","restore_drill_at":"`+
		drilled.Format(time.RFC3339)+`"}`)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("a receipt stamped in another zone was refused: %v", err)
	}
	gate, err := NewSignedBackupGate(config.AttestationPath, public, config.ApprovalID,
		time.Hour, 48*time.Hour, 90*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	if err := gate.Check(context.Background(), config.ApprovalID); err != nil {
		t.Fatalf("the published attestation failed verification: %v", err)
	}
	raw, err := os.ReadFile(config.AttestationPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(raw), drilled.UTC().Format("2006-01-02T15:04:05")) {
		t.Fatalf("the drill instant was not published as UTC: %s", filepath.Base(config.AttestationPath))
	}
}
