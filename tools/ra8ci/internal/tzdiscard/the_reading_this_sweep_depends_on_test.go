// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package tzdiscard

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// What this gate reports is only as good as what it can read. These hold the
// edges of that reading: a line long enough to bury its own finding, a match
// that is already commented out on the line it sits in, an entry in the tree
// that vanishes between the walk and the stat, and a self-test that cannot
// build its own fixture. Each of them is a way the sweep could quietly report
// a clean tree, which is exactly what the floor above it exists to prevent.

// A snippet is evidence, not the file. A very long line is cut so a report
// stays readable, and the cut is by rune so a multi-byte source is never split
// through the middle of a character.
func TestALongLineIsQuotedOnlyAsFarAsItIsUseful(t *testing.T) {
	root := t.TempDir()
	padding := strings.Repeat("x", 400)
	plantSource(t, root, "apps/long.c", "void app_main(void)\n{\n  (void)ra8_tz_secure_boot_verify(); // "+padding+"\n}\n")

	run := sweep(t, root, "apps/long.c")
	if run.code != 1 {
		t.Fatalf("exit = %d, want 1; stderr %q", run.code, run.stderr)
	}
	report := run.stdout + run.stderr
	if strings.Contains(report, strings.Repeat("x", 120)) {
		t.Fatal("a finding quoted the whole of a 400-character line, want the snippet cut")
	}
	if !strings.Contains(report, "(void)ra8_tz_secure_boot_verify()") {
		t.Fatal("the cut removed the finding itself")
	}
	if !strings.Contains(report, "apps/long.c:3:") {
		t.Fatalf("the finding lost its path and line: %q", report)
	}
}

func TestALongLineIsCutOnRunesNotBytes(t *testing.T) {
	root := t.TempDir()
	padding := strings.Repeat("\u00e9", 400)
	plantSource(t, root, "apps/wide.c", "void app_main(void)\n{\n  (void)ra8_tz_secure_boot_verify(); /* "+padding+" */\n}\n")

	run := sweep(t, root, "apps/wide.c")
	if run.code != 1 {
		t.Fatalf("exit = %d, want 1; stderr %q", run.code, run.stderr)
	}
	report := run.stdout + run.stderr
	if strings.Contains(report, "\ufffd") {
		t.Fatal("the snippet was cut through a character")
	}
}

// A discard that is already commented out is not a discard. The detector reads
// the text before the match, so the same call is a finding in code and quiet
// behind a comment opened on that line.
func TestADiscardBehindACommentOnTheSameLineIsNotAFinding(t *testing.T) {
	root := t.TempDir()
	plantSource(t, root, "apps/live.c", "void app_main(void)\n{\n  int n = 0; (void)ra8_tz_secure_boot_verify();\n}\n")
	plantSource(t, root, "apps/dead.c", "void app_main(void)\n{\n  int n = 0; // (void)ra8_tz_secure_boot_verify();\n}\n")
	plantSource(t, root, "apps/block.c", "void app_main(void)\n{\n  int n = 0; /* (void)ra8_tz_secure_boot_verify();\n}\n")

	if run := sweep(t, root, "apps/live.c"); run.code != 1 {
		t.Fatalf("a live discard after other code on its line exited %d, want 1", run.code)
	}
	for _, rel := range []string{"apps/dead.c", "apps/block.c"} {
		if run := sweep(t, root, rel); run.code != 0 {
			t.Fatalf("%s exited %d, want 0; stderr %q", rel, run.code, run.stderr)
		}
	}
}

// A tree is walked and then read, and the two are not one instant. A dangling
// symlink is the cheap standing proof: it is listed by the walk and gone by the
// stat, and the sweep passes over it rather than refusing the whole tree.
func TestAnEntryThatIsNotThereWhenItIsReadIsPassedOver(t *testing.T) {
	root := t.TempDir()
	plantTreeAboveTheFloor(t, root, 1)
	plantSource(t, root, "apps/real.c", boundaryDiscard)
	symlinkTest(t, filepath.Join(root, "apps", "absent.c"), filepath.Join(root, "apps", "ghost.c"))

	run := sweep(t, root)
	if run.code != 1 {
		t.Fatalf("exit = %d, want 1 for the real finding; stderr %q", run.code, run.stderr)
	}
	if strings.Contains(run.stdout+run.stderr, "ghost.c") {
		t.Fatalf("a link to nothing was reported: %q", run.stdout+run.stderr)
	}
	if !strings.Contains(run.stdout+run.stderr, "real.c") {
		t.Fatal("the sweep that passed over the link also lost the file beside it")
	}
}

// The self-test builds its own fixture, so it has somewhere to fail before it
// has judged anything. It answers 1 and says why, rather than reporting that
// every case passed over a fixture it never wrote.
func TestASelfTestThatCannotBuildItsFixtureFailsLoudly(t *testing.T) {
	sealed := filepath.Join(t.TempDir(), "sealed")
	if err := os.Mkdir(sealed, 0o500); err != nil {
		t.Fatal(err)
	}
	t.Setenv("TMPDIR", sealed)
	if _, err := os.MkdirTemp("", "probe-"); err == nil {
		t.Skip("this box lets a sealed temporary directory be written")
	}

	run := sweep(t, t.TempDir(), "--selftest")
	if run.code != 1 {
		t.Fatalf("exit = %d, want 1; stdout %q stderr %q", run.code, run.stdout, run.stderr)
	}
	if !strings.Contains(run.stderr, "self-test fixture") {
		t.Fatalf("stderr = %q, want the fixture named as the failure", run.stderr)
	}
	if strings.Contains(run.stdout, "all cases pass") {
		t.Fatal("a self-test that never wrote its fixture reported that every case passed")
	}
}
