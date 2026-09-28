// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The backup signing key is what lets the monitor swear a backup happened.
// Generating it has to fail loudly and leave nothing half-written, because a
// half-written pair is a gate keyed to a private half nobody holds.

// sealedRoot returns a directory nothing can be created in, along with a
// directory that still accepts writes.
func sealedRoot(t *testing.T) (sealed, open string) {
	t.Helper()
	if os.Geteuid() == 0 {
		t.Skip("a sealed directory is still writable by root")
	}
	root := t.TempDir()
	sealed = filepath.Join(root, "sealed")
	open = filepath.Join(root, "open")
	for _, path := range []string{sealed, open} {
		if err := os.Mkdir(path, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Chmod(sealed, 0o500); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o755) })
	return sealed, open
}

// A directory that passes every ownership check and still refuses the file
// is reported as the creation failure it is.
func TestCreateBackupSigningKeyPairReportsAKeyItCannotCreate(t *testing.T) {
	sealed, open := sealedRoot(t)
	err := CreateBackupSigningKeyPair(filepath.Join(sealed, "backup.key"), filepath.Join(open, "backup.pub"))
	if err == nil {
		t.Fatal("a key was created in a sealed directory")
	}
	if !strings.Contains(err.Error(), "create backup key file") {
		t.Fatalf("a sealed private path = %v", err)
	}
	if _, statErr := os.Lstat(filepath.Join(open, "backup.pub")); statErr == nil {
		t.Fatal("the public half was written after the private half failed")
	}
}

// The private half is written first, so a public half that cannot be written
// has to take the private half back out with it. Left behind, it is a secret
// on disk for a gate that was never armed.
func TestCreateBackupSigningKeyPairLeavesNoHalfWrittenPair(t *testing.T) {
	sealed, open := sealedRoot(t)
	private := filepath.Join(open, "backup.key")
	err := CreateBackupSigningKeyPair(private, filepath.Join(sealed, "backup.pub"))
	if err == nil {
		t.Fatal("a pair was created with a sealed public path")
	}
	if _, statErr := os.Lstat(private); statErr == nil {
		t.Fatal("the private half was left on disk after the public half failed")
	}
}

// Both halves are owner-only and decode to a usable ed25519 pair, and the
// second attempt at the same paths refuses rather than rotating the key out
// from under whoever already trusts it.
func TestCreateBackupSigningKeyPairWritesOwnerOnlyHalvesOnce(t *testing.T) {
	root := t.TempDir()
	private := filepath.Join(root, "backup.key")
	public := filepath.Join(root, "backup.pub")
	if err := CreateBackupSigningKeyPair(private, public); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{private, public} {
		info, err := os.Lstat(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.Mode().Perm() != 0o600 {
			t.Fatalf("%s mode = %v", filepath.Base(path), info.Mode().Perm())
		}
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(raw))); err != nil {
			t.Fatalf("%s is not base64: %v", filepath.Base(path), err)
		}
	}
	if err := CreateBackupSigningKeyPair(private, public); err == nil {
		t.Fatal("an existing signing key was replaced")
	}
}

// Paths that are not two distinct, absolute, already-clean files under a
// real protected directory are refused before any key is generated.
func TestCreateBackupSigningKeyPairRefusesPathsItCannotTrust(t *testing.T) {
	root := t.TempDir()
	loose := filepath.Join(root, "loose")
	if err := os.Mkdir(loose, 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(loose, 0o777); err != nil {
		t.Fatal(err)
	}
	linked := filepath.Join(root, "linked")
	if err := os.Symlink(root, linked); err != nil {
		t.Fatal(err)
	}
	good := filepath.Join(root, "backup.pub")

	for name, pair := range map[string][2]string{
		"a relative private path": {"backup.key", good},
		"a relative public path":  {filepath.Join(root, "backup.key"), "backup.pub"},
		// filepath.Join cleans as it joins, so an unclean path has to be
		// built by hand to reach the check at all.
		"an unclean path":             {root + "/./backup.key", good},
		"one path for both halves":    {good, good},
		"a parent that is not there":  {filepath.Join(root, "absent", "backup.key"), good},
		"a parent anyone can write":   {filepath.Join(loose, "backup.key"), good},
		"a parent reached by symlink": {filepath.Join(linked, "backup.key"), good},
	} {
		if err := CreateBackupSigningKeyPair(pair[0], pair[1]); err == nil {
			t.Fatalf("%s was accepted", name)
		}
		if _, statErr := os.Lstat(good); statErr == nil {
			t.Fatalf("%s still wrote a key", name)
		}
	}
}

// The stanza alphabet is what keeps an attestation's own field names from
// carrying anything a reader has to escape.
func TestBackupStanzaHoldsToItsAlphabet(t *testing.T) {
	for _, value := range []string{"a", "A", "0", "nightly", "nightly_full", "nightly-full", strings.Repeat("a", 64)} {
		if !validBackupStanza(value) {
			t.Fatalf("%q was refused", value)
		}
	}
	for name, value := range map[string]string{
		"empty":              "",
		"too long":           strings.Repeat("a", 65),
		"leading space":      " nightly",
		"trailing space":     "nightly ",
		"inner space":        "nightly full",
		"leading underscore": "_nightly",
		"leading hyphen":     "-nightly",
		"a dot":              "nightly.full",
		"a slash":            "nightly/full",
		"a newline":          "nightly\n",
		"not ascii":          "nightlyé",
	} {
		if validBackupStanza(value) {
			t.Fatalf("%s was accepted", name)
		}
	}
}
