// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The backup signing key is what lets the monitor swear a backup happened.
// Generating it has to fail loudly and leave nothing half-written, because a
// half-written pair is a gate keyed to a private half nobody holds.

// Paths that are not two distinct, absolute, already-clean files under a
// real protected directory are refused before any key is generated.
func TestCreateBackupSigningKeyPairRefusesPathsItCannotTrust(t *testing.T) {
	root := t.TempDir()
	linked := filepath.Join(root, "linked")
	symlinkTest(t, root, linked)
	good := filepath.Join(root, "backup.pub")

	for name, pair := range map[string][2]string{
		"a relative private path": {"backup.key", good},
		"a relative public path":  {filepath.Join(root, "backup.key"), "backup.pub"},
		// filepath.Join cleans as it joins, so an unclean path has to be
		// built by hand to reach the check at all.
		"an unclean path":             {root + "/./backup.key", good},
		"one path for both halves":    {good, good},
		"a parent that is not there":  {filepath.Join(root, "absent", "backup.key"), good},
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
