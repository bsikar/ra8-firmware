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
)

// Every file the backup monitor reads is inspected with Lstat before it is
// opened, and the two can disagree: a file whose mode passes every policy
// check can still refuse to open. The monitor answers each of those with its
// own message, which is what tells an operator whether the file's permissions
// are wrong or the file itself is unreadable.

// seal makes a file unopenable while leaving the mode the policy checks read
// unchanged, and skips the case when this process can read it anyway.
func seal(t *testing.T, path string) {
	t.Helper()
	if err := os.Chmod(path, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(path, 0o600) })
	if _, err := os.ReadFile(path); err == nil {
		t.Skip("this process can read a sealed file")
	}
}

// A configuration that never named its inputs properly is refused before a
// single file is touched, so a monitor run cannot half-happen against a
// half-specified config.
func TestAnIncompleteMonitorConfigurationIsRefusedBeforeAnyFile(t *testing.T) {
	valid, _ := backupMonitorFixture(t, 0o750)

	for name, break_ := range map[string]func(c *BackupMonitorConfig){
		"a relative pgBackRest path":  func(c *BackupMonitorConfig) { c.PgBackRestPath = "pgbackrest" },
		"a relative signing key":      func(c *BackupMonitorConfig) { c.PrivateKeyPath = "signing.key" },
		"a relative drill receipt":    func(c *BackupMonitorConfig) { c.RestoreDrillPath = "restore.json" },
		"a relative attestation path": func(c *BackupMonitorConfig) { c.AttestationPath = "out/backup.json" },
		"an empty stanza":             func(c *BackupMonitorConfig) { c.Stanza = "" },
		"a stanza with a slash":       func(c *BackupMonitorConfig) { c.Stanza = "ra8ci/prod" },
		"an approval that is not an id": func(c *BackupMonitorConfig) {
			c.ApprovalID = "approval-1"
		},
		"no approval at all": func(c *BackupMonitorConfig) { c.ApprovalID = "" },
	} {
		t.Run(name, func(t *testing.T) {
			config := valid
			break_(&config)

			err := RefreshBackupAttestation(context.Background(), config)
			if err == nil || !strings.Contains(err.Error(), "invalid backup monitor configuration") {
				t.Fatalf("answered %v, want a configuration refusal", err)
			}
		})
	}
}

// A drill receipt whose mode passes policy but which will not open is named as
// unreadable, not as badly permissioned. The two have different fixes.
func TestADrillReceiptThatWillNotOpenIsNamedAsSuch(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	seal(t, config.RestoreDrillPath)

	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil || err.Error() != "open restore drill receipt" {
		t.Fatalf("answered %v, want the drill receipt named as unopenable", err)
	}
}

// The same for the signing key, and it is reached only because the drill
// receipt ahead of it was fine: the monitor validates its inputs in order, so
// this also pins that the key is read after the receipt rather than before.
func TestASigningKeyThatWillNotOpenIsNamedAsSuch(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	seal(t, config.PrivateKeyPath)

	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil || err.Error() != "open backup signing key" {
		t.Fatalf("answered %v, want the signing key named as unopenable", err)
	}
}

// The gate's own key decides which signatures it will accept, so the loader
// holds it to the same rule and distinguishes the same two failures.
func TestAPublicKeyThatWillNotOpenIsNamedAsSuch(t *testing.T) {
	public, _, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "backup.pub")
	if err := os.WriteFile(path, []byte(base64.StdEncoding.EncodeToString(public)), 0o644); err != nil {
		t.Fatal(err)
	}

	if loaded, err := LoadBackupPublicKey(path); err != nil || !loaded.Equal(public) {
		t.Fatalf("a readable key was refused: %v", err)
	}
	seal(t, path)
	if _, err := LoadBackupPublicKey(path); err == nil || err.Error() != "open backup public key" {
		t.Fatalf("answered %v, want the public key named as unopenable", err)
	}
}
