// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The key pair is generated once and never replaced, so every refusal here is
// the last chance to stop a monitor from being keyed to the wrong material.
// Each one names what is wrong rather than reporting a generic failure.

// sealDir makes a directory unwritable while leaving the permission bits the
// policy reads acceptable, and skips the case when this process can write it
// anyway.
func sealDir(t *testing.T, path string) {
	t.Helper()
	if err := os.Chmod(path, 0o500); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(path, 0o700) })
	probe := filepath.Join(path, ".probe")
	if err := os.WriteFile(probe, []byte("x"), 0o600); err == nil {
		_ = os.Remove(probe)
		t.Skip("this process can write a sealed directory")
	}
}

// Paths that were never usable as key paths are refused before any key
// material exists, so a bad invocation cannot leave half a pair behind.
func TestUnusableKeyPathsAreRefusedBeforeAnyKeyExists(t *testing.T) {
	root := t.TempDir()
	good := filepath.Join(root, "signing.key")

	for name, paths := range map[string][2]string{
		"a relative private path": {"signing.key", filepath.Join(root, "signing.pub")},
		"a relative public path":  {good, "signing.pub"},
		// filepath.Join cleans as it joins, so an uncleaned path has to be
		// built by hand to reach the rule that refuses one.
		"an uncleaned private path":   {root + "/keys/../signing.key", filepath.Join(root, "signing.pub")},
		"an uncleaned public path":    {good, root + "/keys/../signing.pub"},
		"one path used for the pair":  {good, good},
		"a private path with a trail": {good + "/", filepath.Join(root, "signing.pub")},
	} {
		t.Run(name, func(t *testing.T) {
			err := CreateBackupSigningKeyPair(paths[0], paths[1])
			if err == nil || err.Error() != "invalid backup signing key paths" {
				t.Fatalf("answered %v, want the paths refused", err)
			}
		})
	}
}

// A parent that is not a protected real directory is refused, and the two
// halves of that rule carry different messages: whether the directory exists
// at all, and whether anyone else can write it.
func TestAnUnprotectedParentIsRefused(t *testing.T) {
	root := t.TempDir()
	absent := filepath.Join(root, "absent", "signing.key")
	if err := CreateBackupSigningKeyPair(absent, filepath.Join(root, "signing.pub")); err == nil ||
		err.Error() != "backup signing key parent must be a protected real directory" {
		t.Fatalf("answered %v, want the missing parent named", err)
	}

	open := filepath.Join(root, "open")
	if err := os.Mkdir(open, 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(open, 0o777); err != nil {
		t.Fatal(err)
	}
	if err := CreateBackupSigningKeyPair(filepath.Join(open, "signing.key"), filepath.Join(open, "signing.pub")); err == nil ||
		err.Error() != "backup signing key parent must not be group or world writable" {
		t.Fatalf("answered %v, want the writable parent named", err)
	}
}

// Existing material is never replaced, whichever half of the pair is already
// there, and the refusal happens before a new key is generated.
func TestExistingKeyMaterialIsNeverReplaced(t *testing.T) {
	for _, existing := range []string{"signing.key", "signing.pub"} {
		t.Run(existing, func(t *testing.T) {
			root := t.TempDir()
			private := filepath.Join(root, "signing.key")
			public := filepath.Join(root, "signing.pub")
			planted := filepath.Join(root, existing)
			if err := os.WriteFile(planted, []byte("theirs\n"), 0o600); err != nil {
				t.Fatal(err)
			}

			err := CreateBackupSigningKeyPair(private, public)
			if err == nil || err.Error() != "refusing to replace existing backup signing key" {
				t.Fatalf("answered %v, want the existing key protected", err)
			}
			kept, readErr := os.ReadFile(planted)
			if readErr != nil || string(kept) != "theirs\n" {
				t.Fatalf("the existing key was disturbed: %q %v", kept, readErr)
			}
		})
	}
}

// A directory nobody else can write can still refuse this process, and that
// is reported as a creation failure rather than as a policy refusal.
func TestAKeyThatCannotBeCreatedIsNamedAsSuch(t *testing.T) {
	root := t.TempDir()
	sealDir(t, root)

	err := CreateBackupSigningKeyPair(filepath.Join(root, "signing.key"), filepath.Join(root, "signing.pub"))
	if err == nil || err.Error() != "create backup key file without replacement" {
		t.Fatalf("answered %v, want the creation failure named", err)
	}
}

// When the public half cannot be written the private half is taken back, so a
// failed generation never leaves a lone private key at a path the next
// attempt would then refuse to replace.
func TestAPrivateKeyIsTakenBackWhenThePublicHalfFails(t *testing.T) {
	root := t.TempDir()
	keys := filepath.Join(root, "keys")
	published := filepath.Join(root, "published")
	for _, directory := range []string{keys, published} {
		if err := os.Mkdir(directory, 0o700); err != nil {
			t.Fatal(err)
		}
	}
	private := filepath.Join(keys, "signing.key")
	sealDir(t, published)

	if err := CreateBackupSigningKeyPair(private, filepath.Join(published, "signing.pub")); err == nil {
		t.Fatal("the pair was reported created")
	}
	if _, err := os.Lstat(private); !os.IsNotExist(err) {
		t.Fatalf("the private half was left behind: %v", err)
	}
}

// The monitor reads pgBackRest's own report, so a response that is empty,
// oversized, or carries a timestamp that is not a completed second is refused
// rather than aged as evidence.
func TestAnUnusablePgBackRestResponseIsRefused(t *testing.T) {
	for name, response := range map[string]string{
		"an empty response":                "",
		"a stop that is not a unix second": `[{"name":"ra8ci","backup":[{"type":"full","timestamp":{"stop":1.5}}]}]`,
		"a stop at the epoch":              `[{"name":"ra8ci","backup":[{"type":"full","timestamp":{"stop":0}}]}]`,
		"a stop before the epoch":          `[{"name":"ra8ci","backup":[{"type":"full","timestamp":{"stop":-1}}]}]`,
	} {
		t.Run(name, func(t *testing.T) {
			_, err := ParseLatestFullBackupInfo([]byte(response), "ra8ci")
			if err == nil {
				t.Fatal("an unusable response was parsed")
			}
		})
	}

	if _, err := ParseLatestFullBackupInfo([]byte(strings.Repeat("x", (8<<20)+1)), "ra8ci"); err == nil ||
		err.Error() != "invalid pgBackRest info response" {
		t.Fatalf("answered %v, want an oversized response refused", err)
	}
}
