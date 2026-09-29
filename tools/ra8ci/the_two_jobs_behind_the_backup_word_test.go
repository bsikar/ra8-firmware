// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// `ra8ci backup` is two quite different jobs behind one word: keygen mints the
// pair that signs backup attestations, and refresh writes an attestation with
// it. Both read their paths from the environment rather than from arguments,
// so the command's own job is to tell an unconfigured operator which of the
// two they asked for and why it would not run, rather than failing somewhere
// inside the monitor with no clue which word caused it.

func TestBackupKeygenMintsAPairAtThePathsTheEnvironmentNames(t *testing.T) {
	home := t.TempDir()
	private := filepath.Join(home, "backup.key")
	public := filepath.Join(home, "backup.pub")
	t.Setenv("RA8CI_BACKUP_SIGNING_KEY", private)
	t.Setenv("RA8CI_BACKUP_PUBLIC_KEY_FILE", public)

	if err := backupCommand(context.Background(), []string{"keygen"}); err != nil {
		t.Fatalf("keygen refused a writable pair of paths: %v", err)
	}

	// The private half is the whole secret, so it may not be readable by
	// anyone else on the box.
	info, err := os.Lstat(private)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("private key mode=%v; want owner-only", info.Mode().Perm())
	}
	for _, path := range []string{private, public} {
		body, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(body))); err != nil {
			t.Fatalf("%s is not the key it claims to be: %v", filepath.Base(path), err)
		}
	}
}

func TestBackupKeygenRefusesToReplaceAPairThatIsAlreadyThere(t *testing.T) {
	home := t.TempDir()
	private := filepath.Join(home, "backup.key")
	public := filepath.Join(home, "backup.pub")
	t.Setenv("RA8CI_BACKUP_SIGNING_KEY", private)
	t.Setenv("RA8CI_BACKUP_PUBLIC_KEY_FILE", public)

	if err := backupCommand(context.Background(), []string{"keygen"}); err != nil {
		t.Fatal(err)
	}
	minted, err := os.ReadFile(private)
	if err != nil {
		t.Fatal(err)
	}
	// A second keygen overwriting the first would invalidate every
	// attestation already signed, silently.
	if err := backupCommand(context.Background(), []string{"keygen"}); err == nil {
		t.Fatal("keygen replaced an existing pair")
	}
	after, err := os.ReadFile(private)
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != string(minted) {
		t.Fatal("the refused keygen still changed the key on disk")
	}
}

func TestBackupKeygenNamesTheJobWhenThePathsCannotBeTrusted(t *testing.T) {
	// Relative paths are refused inside the monitor; the command has to say
	// which of its two jobs was being attempted when that happened.
	t.Setenv("RA8CI_BACKUP_SIGNING_KEY", "backup.key")
	t.Setenv("RA8CI_BACKUP_PUBLIC_KEY_FILE", "backup.pub")

	err := backupCommand(context.Background(), []string{"keygen"})
	if err == nil {
		t.Fatal("keygen accepted relative paths")
	}
	if !strings.Contains(err.Error(), "create backup signing key pair") {
		t.Fatalf("refusal=%v; want the keygen job named", err)
	}
}

func TestBackupRefreshNamesTheJobWhenNothingIsConfigured(t *testing.T) {
	for _, name := range []string{
		"RA8CI_PGBACKREST_PATH", "RA8CI_BACKUP_SIGNING_KEY", "RA8CI_RESTORE_DRILL_RECEIPT",
		"RA8CI_BACKUP_ATTESTATION_FILE", "RA8CI_BACKUP_APPROVAL_ID", "RA8CI_PGBACKREST_STANZA",
	} {
		t.Setenv(name, "")
	}
	err := backupCommand(context.Background(), []string{"refresh"})
	if err == nil {
		t.Fatal("refresh ran with nothing configured")
	}
	if !strings.Contains(err.Error(), "refresh backup attestation") {
		t.Fatalf("refusal=%v; want the refresh job named", err)
	}
}
