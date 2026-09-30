// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The self-test judges the transliteration table with the table itself, so a
// table that had drifted was judged by its own output and always agreed. Swap
// it and the self-test has to refuse: a gate that rewrites source files must
// not certify itself on a translator that stopped doing what it claims.
func withReplacements(t *testing.T, pairs ...string) {
	t.Helper()
	shipped := replacements
	replacements = strings.NewReplacer(pairs...)
	t.Cleanup(func() { replacements = shipped })
}

func TestASelfTestWhoseTranslatorTouchesPlainASCIIRefuses(t *testing.T) {
	// Nothing above 0x7f here, so a table that rewrites it is rewriting text
	// the gate promised to leave exactly as it found it.
	withReplacements(t, "clean", "muddied")

	if held, _ := runSelfTest(t, t.TempDir()); held {
		t.Fatal("the self-test passed with a table that rewrites plain ASCII")
	}
}

func TestASelfTestWhoseTranslatorLostItsMappingsRefuses(t *testing.T) {
	// An empty table leaves plain ASCII alone, which is exactly why a
	// one-sided self-test would have accepted it: the em dash and the micro
	// sign now fall through to the question mark instead of being translated.
	withReplacements(t)

	if held, _ := runSelfTest(t, t.TempDir()); held {
		t.Fatal("the self-test passed with a table that translates nothing")
	}
}

func TestASelfTestWhoseUnknownRuneStoppedBeingAQuestionMarkRefuses(t *testing.T) {
	// The mappings the earlier cases check are kept, so this reaches the case
	// that fixes what an untranslatable rune becomes. A gate that silently
	// writes some other placeholder into source is the drift this catches.
	withReplacements(t, "\u2014", "--", "\u00b5", "u", "\u2603", "*")

	if held, _ := runSelfTest(t, t.TempDir()); held {
		t.Fatal("the self-test passed with an unknown rune written as something else")
	}
}

// aTemporaryDirectoryWithNoRoomForTheFixture points the temporary directory at
// a path long enough that the self-test's own directory still fits inside the
// kernel's path limit while the fixture file beneath it does not, so the
// directory is created and the fixture cannot be written.
func aTemporaryDirectoryWithNoRoomForTheFixture(t *testing.T) string {
	t.Helper()
	// The directory lands at this depth plus the pattern and its random
	// suffix, which stays inside the limit for every suffix length while
	// fixture.md beneath it is over the limit for all of them.
	const target = 4062
	deep := t.TempDir()
	if len(deep) >= target {
		t.Skipf("the test root is already %d characters deep", len(deep))
	}
	for len(deep) < target {
		remaining := target - len(deep) - 1
		if remaining > 100 {
			remaining = 100
		}
		deep = filepath.Join(deep, strings.Repeat("d", remaining))
	}
	if err := os.MkdirAll(deep, 0o755); err != nil {
		t.Skipf("this filesystem will not hold a %d character path: %v", len(deep), err)
	}
	return deep
}

func TestASelfTestThatCannotWriteItsFixtureRefuses(t *testing.T) {
	deep := aTemporaryDirectoryWithNoRoomForTheFixture(t)
	t.Setenv("TMPDIR", deep)

	// Sanity: a directory can still be made here, so the refusal under test is
	// the fixture write and not the directory the other case already covers.
	made, err := os.MkdirTemp("", "ra8ci-ascii-selftest-")
	if err != nil {
		t.Skipf("no temporary directory can be made at this depth: %v", err)
	}
	defer os.RemoveAll(made)
	if writeErr := os.WriteFile(filepath.Join(made, "fixture.md"), []byte("dash\u2014\n"), 0o600); writeErr == nil {
		t.Skip("this filesystem accepts the fixture path, so the write cannot be made to fail")
	}

	if held, _ := runSelfTest(t, t.TempDir()); held {
		t.Fatal("the self-test passed without a fixture it could write")
	}
}

func TestTheSelfTestStillWatchesTheTableThatShipped(t *testing.T) {
	// The shipped table has to clear the three transliteration cases; the
	// self-test only refuses later, on the live scope a bare directory has
	// nothing to offer. That ordering is what the swaps above rely on.
	_, complaint := runSelfTest(t, t.TempDir())
	if !strings.Contains(complaint, "live derived scope") {
		t.Fatalf("the shipped table did not reach the live scope check: %q", complaint)
	}
}

func TestATranslatedRuneIsCountedBeforeItIsReplaced(t *testing.T) {
	// The count is taken from the original text, so a mapping that expands one
	// rune into several characters still reports one finding, and the check
	// the self-test makes on that count is not accidentally about length.
	clean, count := transliterate("temperature 20\u00b0C\n")
	if clean != "temperature 20 degC\n" || count != 1 {
		t.Fatalf("transliterate = %q, %d", clean, count)
	}
	if _, count := transliterate("\u2264 \u2265 \u2260 \u2192 \u2190 \u00d7 \u00f7"); count != 7 {
		t.Fatalf("seven mapped runes counted as %d", count)
	}
	// An unmapped rune is still counted, which is what makes the question mark
	// a finding rather than a silent substitution.
	if clean, count := transliterate("snowman \u2603"); clean != "snowman ?" || count != 1 {
		t.Fatalf("unmapped rune = %q, %d", clean, count)
	}
}
