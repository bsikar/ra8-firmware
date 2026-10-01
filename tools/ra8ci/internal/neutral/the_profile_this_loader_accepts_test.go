// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A neutral profile is reviewed data that decides when a board may be handed
// to another holder, so the loader refuses anything it cannot prove was
// installed from a reviewed source, and the JSON inspection refuses a
// document whose meaning depends on which duplicate key a parser keeps.
// The exact digest, the group-writable file, the symlink, the trailing
// document and the duplicate top-level key are already held by
// profile_test.go; these are the ones around them.

func writtenProfile(t *testing.T, name string, raw []byte, mode os.FileMode) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, raw, mode); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, mode); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestLoadProfileRefusesAFileItCannotProveWasReviewed(t *testing.T) {
	raw, err := json.Marshal(validProfileFixture())
	if err != nil {
		t.Fatal(err)
	}
	directory := t.TempDir()

	for name, path := range map[string]string{
		"a profile that is not there": filepath.Join(directory, "absent.json"),
		"a directory standing in":     directory,
		"an empty file":               writtenProfile(t, "empty.json", nil, 0600),
		"a world-writable profile":    writtenProfile(t, "open.json", raw, 0606),
		"a profile past the size bound": writtenProfile(t, "huge.json",
			append(raw, []byte(strings.Repeat(" ", MaxProfileBytes))...), 0600),
	} {
		if _, _, err := LoadProfile(path); !errors.Is(err, ErrInvalidProfile) {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// The inspection walks the whole document, so a duplicate key is refused
// wherever it is: nested in an object, or inside an element of an array.
// Encoding/json itself keeps the last one silently, which is exactly the
// ambiguity a reviewed profile may not carry.
func TestProfileJSONRefusesADuplicateKeyAtAnyDepth(t *testing.T) {
	for name, document := range map[string]string{
		"a duplicate nested key":                     `{"a":{"board_id":"one","board_id":"two"}}`,
		"a duplicate key inside an array element":    `{"identity":[{"name":"power","name":"other"}]}`,
		"a duplicate key deep in an array of arrays": `{"a":[[{"x":1,"x":2}]]}`,
		"a duplicate key beside a nested object":     `{"a":{"b":1},"a":{"b":2}}`,
	} {
		decoder := json.NewDecoder(strings.NewReader(document))
		err := inspectProfileJSON(decoder)
		if err == nil || !strings.Contains(err.Error(), "duplicate object key") {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// A document the inspection accepts is not yet a profile: the scalars and
// arrays pass here and are refused later, by the decode and by
// ValidateProfile. Holding that split keeps the inspection honest about
// what it is for.
func TestProfileJSONAcceptsAnyUnambiguousDocument(t *testing.T) {
	for name, document := range map[string]string{
		"a bare number":                `5`,
		"a bare string":                `"ek-ra8d2"`,
		"an empty object":              `{}`,
		"an array of objects":          `[{"a":1},{"a":2}]`,
		"the same key in two siblings": `{"a":{"n":1},"b":{"n":2}}`,
		"an object nested three deep":  `{"a":{"b":{"c":{"d":1}}}}`,
	} {
		decoder := json.NewDecoder(strings.NewReader(document))
		if err := inspectProfileJSON(decoder); err != nil {
			t.Fatalf("%s = %v", name, err)
		}
	}

	path := writtenProfile(t, "scalar.json", []byte(`5`), 0600)
	if _, _, err := LoadProfile(path); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("a bare number loaded as a profile: %v", err)
	}
}

// A truncated document has no closing delimiter to read, so the inspection
// fails on the token rather than reporting a clean walk.
func TestProfileJSONRefusesATruncatedDocument(t *testing.T) {
	for name, document := range map[string]string{
		"an object left open":     `{"a":1`,
		"an array left open":      `[1,2`,
		"a key with no value":     `{"a":`,
		"a non-string object key": "{1:2}",
		"nothing at all":          ``,
	} {
		decoder := json.NewDecoder(strings.NewReader(document))
		if err := inspectProfileJSON(decoder); err == nil {
			t.Fatalf("%s was accepted", name)
		}
	}
}

// Every path a profile names is read relative to a fixed root, so a path
// that could escape it, or that is not a path at all, is refused before
// the profile is ever installed.
func TestProfileRelativePathsCannotEscapeTheirRoot(t *testing.T) {
	held := []string{
		"ttyUSB0",
		"dev/ttyUSB0",
		"class/hwmon/hwmon0/in0_input",
		"a+b/c-d_e.f",
		strings.Repeat("a", 256),
	}
	for _, value := range held {
		if !validRelativePath(value) {
			t.Fatalf("%q was refused", value)
		}
	}

	refused := []string{
		"",
		".",
		"..",
		"../etc/shadow",
		"/dev/ttyUSB0",
		"dev//ttyUSB0",
		"dev/./ttyUSB0",
		"dev/../ttyUSB0",
		"dev/ttyUSB0/",
		"dev/tty USB0",
		"dev/ttyUSB0\n",
		"dev/tty*",
		strings.Repeat("a", 257),
	}
	for _, value := range refused {
		if validRelativePath(value) {
			t.Fatalf("%q was accepted", value)
		}
	}
}

// A key ID and a board ID both name something an operator will read back
// out of a receipt, so they are held to the same alphabet and to their own
// bound: 64 for a key, 128 for a board.
func TestReceiptIdentifiersAreHeldToTheirAlphabetAndBound(t *testing.T) {
	for _, value := range []string{"a", "neutral-key.1", "A_B-C.9", strings.Repeat("k", 64)} {
		if !validKeyID(value) {
			t.Fatalf("key ID %q was refused", value)
		}
	}
	for _, value := range []string{"", strings.Repeat("k", 65), "key/1", "key 1", "key:1", "kéy", "key\n"} {
		if validKeyID(value) {
			t.Fatalf("key ID %q was accepted", value)
		}
	}

	for _, value := range []string{"a", "ek-ra8d2", strings.Repeat("b", 128), strings.Repeat("k", 65)} {
		if !validBoardID(value) {
			t.Fatalf("board ID %q was refused", value)
		}
	}
	for _, value := range []string{"", strings.Repeat("b", 129), "board/1", "board 1", "bóard", "board\t"} {
		if validBoardID(value) {
			t.Fatalf("board ID %q was accepted", value)
		}
	}
}
