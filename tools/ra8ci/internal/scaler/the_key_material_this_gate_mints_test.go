// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The backup signing key is the root of the off-VM backup gate, so the only
// interesting question about its creation is what it REFUSES: a path it
// cannot reason about, a parent directory another account can write, and any
// existing material. These hold those refusals and the rollback behind them.

func keyPaths(t *testing.T) (string, string) {
	t.Helper()
	dir := t.TempDir()
	return filepath.Join(dir, "private.key"), filepath.Join(dir, "public.key")
}

func TestBackupKeyPathsMustBeAbsoluteCleanAndDistinct(t *testing.T) {
	private, public := keyPaths(t)
	dir := filepath.Dir(private)
	for name, pair := range map[string][2]string{
		"a relative private path":  {"relative/private.key", public},
		"a relative public path":   {private, "relative/public.key"},
		"an unclean private path":  {filepath.Join(dir, "..", filepath.Base(dir), "private.key") + "/.", public},
		"a traversing public path": {private, dir + "/../" + filepath.Base(dir) + "/public.key"},
		"one path for both":        {private, private},
	} {
		err := CreateBackupSigningKeyPair(pair[0], pair[1])
		if err == nil || !strings.Contains(err.Error(), "invalid backup signing key paths") {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if entries, _ := os.ReadDir(dir); len(entries) != 0 {
		t.Fatalf("a refused path still wrote %d file(s)", len(entries))
	}
}

func TestBackupKeyParentMustBeARealProtectedDirectory(t *testing.T) {
	private, public := keyPaths(t)
	missing := filepath.Join(filepath.Dir(private), "absent", "private.key")
	if err := CreateBackupSigningKeyPair(missing, public); err == nil ||
		!strings.Contains(err.Error(), "protected real directory") {
		t.Fatalf("an absent parent = %v", err)
	}

	// A file standing where the parent directory should be is not a
	// directory at all, and is refused as such rather than opened.
	notADirectory := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(notADirectory, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := CreateBackupSigningKeyPair(filepath.Join(notADirectory, "private.key"), public); err == nil ||
		!strings.Contains(err.Error(), "protected real directory") {
		t.Fatalf("a file as parent = %v", err)
	}
}

// Exclusive creation cannot stop an account that can write the directory from
// renaming the new key away and leaving its own pair behind, so a group or
// world writable parent is refused before any key is generated.
func TestBackupKeyParentMayNotBeGroupOrWorldWritable(t *testing.T) {
	for name, mode := range map[string]os.FileMode{
		"group writable": 0o770,
		"world writable": 0o707,
	} {
		dir := t.TempDir()
		if err := os.Chmod(dir, mode); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = os.Chmod(dir, 0o700) })
		private, public := filepath.Join(dir, "private.key"), filepath.Join(dir, "public.key")
		err := CreateBackupSigningKeyPair(private, public)
		if err == nil || !strings.Contains(err.Error(), "must not be group or world writable") {
			t.Fatalf("%s parent = %v", name, err)
		}
		if _, statErr := os.Lstat(private); statErr == nil {
			t.Fatalf("%s parent still received a key", name)
		}
	}
}

// Existing material is never replaced, from either side of the pair.
func TestExistingBackupKeyMaterialIsNeverReplaced(t *testing.T) {
	for name, existing := range map[string]int{"the private key": 0, "the public key": 1} {
		private, public := keyPaths(t)
		paths := [2]string{private, public}
		if err := os.WriteFile(paths[existing], []byte("held\n"), 0o600); err != nil {
			t.Fatal(err)
		}
		err := CreateBackupSigningKeyPair(private, public)
		if err == nil || !strings.Contains(err.Error(), "refusing to replace existing backup signing key") {
			t.Fatalf("%s already present = %v", name, err)
		}
		held, readErr := os.ReadFile(paths[existing])
		if readErr != nil || string(held) != "held\n" {
			t.Fatalf("%s changed: %q %v", name, held, readErr)
		}
		if _, statErr := os.Lstat(paths[1-existing]); statErr == nil {
			t.Fatalf("%s refusal still minted the other half", name)
		}
	}
}

// A pair is written whole or not at all: if the public half cannot be
// created, the private half is removed rather than left as unpaired secret
// material nothing can verify against.
func TestAHalfWrittenPairLeavesNoPrivateKeyBehind(t *testing.T) {
	privateDir, sealedDir := t.TempDir(), t.TempDir()
	if err := os.Chmod(sealedDir, 0o500); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealedDir, 0o700) })
	private := filepath.Join(privateDir, "private.key")
	public := filepath.Join(sealedDir, "public.key")
	if handle, err := os.OpenFile(filepath.Join(sealedDir, "probe"), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600); err == nil {
		_ = handle.Close()
		t.Skip("this account can write a read-only directory")
	}
	err := CreateBackupSigningKeyPair(private, public)
	if err == nil || !strings.Contains(err.Error(), "create backup key file without replacement") {
		t.Fatalf("a sealed public parent = %v", err)
	}
	if _, statErr := os.Lstat(private); statErr == nil {
		t.Fatal("the private key survived a failed pair")
	}
}

// The stanza names a backup in paths and receipts, so it has to be a plain
// identifier: no separators, no spaces, no leading punctuation.
func TestABackupStanzaIsAPlainIdentifier(t *testing.T) {
	for name, held := range map[string]struct {
		value string
		want  bool
	}{
		"a word":                    {"nightly", true},
		"digits and case":           {"Nightly2026", true},
		"inner underscore and dash": {"nightly_lab-01", true},
		"a single letter":           {"n", true},
		"a single digit":            {"7", true},
		"exactly sixty-four":        {strings.Repeat("a", 64), true},
		"sixty-five":                {strings.Repeat("a", 65), false},
		"empty":                     {"", false},
		"leading underscore":        {"_nightly", false},
		"leading dash":              {"-nightly", false},
		"a leading space":           {" nightly", false},
		"a trailing space":          {"nightly ", false},
		"an inner space":            {"nightly lab", false},
		"a path separator":          {"nightly/lab", false},
		"a dot":                     {"nightly.lab", false},
		"a newline":                 {"nightly\n", false},
		"beyond ASCII":              {"nightlyé", false},
	} {
		if got := validBackupStanza(held.value); got != held.want {
			t.Fatalf("%s: validBackupStanza(%q) = %v", name, held.value, got)
		}
	}
}
