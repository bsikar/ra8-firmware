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
