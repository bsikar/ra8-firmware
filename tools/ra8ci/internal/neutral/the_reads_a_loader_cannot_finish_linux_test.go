// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Both loaders judge a path by what they can see of it before opening it,
// and both then have to survive the open and the read failing anyway. A
// keyring or a profile that cannot be finished must refuse, because the
// alternative is a plane that starts with a keyring it never read.

// unreadable plants a file that passes every check made on its path (a
// bounded regular file not writable by group or other) and still cannot be
// opened. The mode is restored on cleanup, without which the temporary
// directory cannot be removed.
func unreadable(t *testing.T, name, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("plant %s: %v", name, err)
	}
	if err := os.Chmod(path, 0); err != nil {
		t.Fatalf("seal %s: %v", name, err)
	}
	t.Cleanup(func() {
		if err := os.Chmod(path, 0o600); err != nil {
			t.Errorf("unseal %s: %v", name, err)
		}
	})
	return path
}

// A keyring whose path looks sound and whose contents cannot be read is
// refused at the open. Mode 0 clears the group and other write bits the path
// check looks for, so this is the one shape that passes that check and still
// fails.
func TestAKeyringThatCannotBeOpenedIsRefused(t *testing.T) {
	path := unreadable(t, "keyring.json", `{"schema_version":1,"agents":[]}`)
	verifier, err := LoadVerifierFile(path)
	if err == nil {
		t.Fatal("a keyring that cannot be opened was loaded")
	}
	if verifier != nil {
		t.Fatalf("a refused keyring still handed back a verifier: %v", verifier)
	}
	if !strings.Contains(err.Error(), "open board-agent verifier keyring") {
		t.Fatalf("err = %v, want the open named", err)
	}
}

// A profile that cannot be read is refused as an invalid profile rather than
// as a missing one, so an operator is pointed at the file they installed
// rather than at a path they would find is there.
func TestAProfileThatCannotBeOpenedIsRefused(t *testing.T) {
	path := unreadable(t, "profile.json", soundProfileDocument(t))
	if _, _, err := LoadProfile(path); err == nil {
		t.Fatal("a profile that cannot be opened was loaded")
	}
}

// A profile whose JSON stops mid-document is refused by the inspection pass
// rather than by the decode that follows it. The inspection is what walks
// the document for duplicate keys, so it is the pass that meets a truncation
// first, and its error has to travel rather than be swallowed as an empty
// profile.
func TestAProfileWhoseJSONStopsPartWayIsRefused(t *testing.T) {
	for name, document := range map[string]string{
		"an object left open":        `{"schema_version":1`,
		"an array left open":         `{"schema_version":1,"devices":[`,
		"a value that never arrives": `{"schema_version":1,"devices":`,
		"nothing but an opening":     `{`,
	} {
		t.Run(name, func(t *testing.T) {
			path := plantProfile(t, document)
			profile, digest, err := LoadProfile(path)
			if err == nil {
				t.Fatalf("a truncated profile was loaded: %v", profile)
			}
			if !errors.Is(err, ErrInvalidProfile) {
				t.Fatalf("err = %v, want it refused as an invalid profile", err)
			}
			if digest != "" {
				t.Fatalf("a refused profile was digested as %q", digest)
			}
		})
	}
}
