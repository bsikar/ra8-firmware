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

// The monitor's inputs are judged by the directory tests beside this file.
// This one is about the last step instead: publishing the signed envelope.
// A publication that half happens is worse than one that does not happen at
// all, since the verifier reads whatever is at the path, so every refusal
// here is also asserted to have left nothing behind.

func attestationLeftovers(t *testing.T, directory string) []string {
	t.Helper()
	entries, err := os.ReadDir(directory)
	if err != nil {
		t.Fatalf("read attestation directory: %v", err)
	}
	var stray []string
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".ra8ci-backup-attestation-") {
			stray = append(stray, entry.Name())
		}
	}
	return stray
}

// The publication renames into the directory, so a symlink there is a
// redirection of where the attestation lands. Lstat is what catches it.
func TestAnAttestationDirectoryThatIsASymlinkIsRefused(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	real := filepath.Join(filepath.Dir(filepath.Dir(config.AttestationPath)), "elsewhere")
	if err := os.Mkdir(real, 0o750); err != nil {
		t.Fatal(err)
	}
	linked := filepath.Join(filepath.Dir(filepath.Dir(config.AttestationPath)), "linked")
	symlinkTest(t, real, linked)
	config.AttestationPath = filepath.Join(linked, "backup.json")
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("a symlinked attestation directory was accepted")
	}
	if !strings.Contains(err.Error(), "attestation directory must be a protected real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
	if entries, readErr := os.ReadDir(real); readErr == nil && len(entries) != 0 {
		t.Fatalf("a refused publication wrote through the symlink: %v", entries)
	}
}

func TestAnAttestationDirectoryThatIsAFileIsRefused(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	plain := filepath.Join(filepath.Dir(filepath.Dir(config.AttestationPath)), "not-a-directory")
	if err := os.WriteFile(plain, []byte("attestations do not go here\n"), 0o640); err != nil {
		t.Fatal(err)
	}
	config.AttestationPath = filepath.Join(plain, "backup.json")
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("a file standing in for the attestation directory was accepted")
	}
	if !strings.Contains(err.Error(), "attestation directory must be a protected real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
	raw, err := os.ReadFile(plain)
	if err != nil {
		t.Fatal(err)
	}
	if string(raw) != "attestations do not go here\n" {
		t.Fatal("a refused publication overwrote the file at the directory's path")
	}
}

// The rename is the one step that can fail after the envelope is signed. A
// directory sitting at the attestation path is the honest way to reach it,
// and the refusal keeps the underlying reason rather than flattening it.
func TestAnAttestationPathHeldByADirectoryIsReportedAsAFailedPublication(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := os.Mkdir(config.AttestationPath, 0o750); err != nil {
		t.Fatal(err)
	}
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("publishing over a directory was accepted")
	}
	if !strings.Contains(err.Error(), "publish backup attestation") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
	info, statErr := os.Lstat(config.AttestationPath)
	if statErr != nil || !info.IsDir() {
		t.Fatal("the directory at the attestation path did not survive the refusal")
	}
	if stray := attestationLeftovers(t, filepath.Dir(config.AttestationPath)); len(stray) != 0 {
		t.Fatalf("a failed publication left a temporary behind: %v", stray)
	}
}

// Nothing is signed on a command that did not answer. The monitor reports the
// command rather than the backup, so an operator reads it as a monitor fault
// and not as a missing backup.
func TestAPgBackRestThatExitsNonZeroSignsNothing(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := rewritePgBackRestFixture(t, config.PgBackRestPath, "#!/bin/sh\nexit 3\n", 0o750); err != nil {
		t.Fatal(err)
	}
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("a failing pgBackRest was accepted")
	}
	if !strings.Contains(err.Error(), "pgBackRest info command failed") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
	if _, statErr := os.Stat(config.AttestationPath); !os.IsNotExist(statErr) {
		t.Fatal("a failing command still published an attestation")
	}
}

// A command that answers with something other than this stanza's full backup
// is refused by the parser, and the monitor hands that reason up unchanged
// rather than replacing it with one of its own.
func TestAPgBackRestAnsweringAnotherStanzaSignsNothing(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	script := "#!/bin/sh\nprintf '[{\"name\":\"other\",\"backup\":[{\"type\":\"full\",\"timestamp\":{\"stop\":1}}]}]'\n"
	if err := rewritePgBackRestFixture(t, config.PgBackRestPath, script, 0o750); err != nil {
		t.Fatal(err)
	}
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("a foreign stanza was accepted")
	}
	if _, statErr := os.Stat(config.AttestationPath); !os.IsNotExist(statErr) {
		t.Fatal("a foreign stanza still published an attestation")
	}
}
