// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"

	"strings"
	"testing"
)

// Every file the backup monitor reads is inspected with Lstat before it is
// opened, and the two can disagree: a file whose mode passes every policy
// check can still refuse to open. The monitor answers each of those with its
// own message, which is what tells an operator whether the file's permissions
// are wrong or the file itself is unreadable.

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
