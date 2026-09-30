// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import (
	"bytes"
	"context"
	"os"
	"regexp"
	"strings"
	"testing"
)

// The self-test is this gate's claim that it still reads a README table and
// still honours the gitignore carve-out. The claim is only worth having if a
// reader gone blind actually breaks it, so this blinds the pattern that lifts
// a subdirectory name out of a row and watches the self-test refuse.

// withRowNamePattern swaps the pattern that reads a name out of a table row.
func withRowNamePattern(t *testing.T, pattern *regexp.Regexp) {
	t.Helper()
	kept := rowNamePattern
	rowNamePattern = pattern
	t.Cleanup(func() { rowNamePattern = kept })
}

func ranSelfTest(t *testing.T) (int, string, string) {
	t.Helper()
	if _, err := os.Stat(trustedGit); err != nil {
		t.Skipf("the self-test needs %s: %v", trustedGit, err)
	}
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

func TestASelfTestWhoseRowReaderWentBlindFails(t *testing.T) {
	withRowNamePattern(t, regexp.MustCompile(`^never-written-in-a-readme-row$`))

	code, stdout, stderr := ranSelfTest(t)

	if code != exitDrift {
		t.Fatalf("exit = %d, want %d; stderr = %q", code, exitDrift, stderr)
	}
	if !strings.Contains(stderr, "selftest FAILED:") {
		t.Fatalf("stderr = %q, want the self-test refusal", stderr)
	}
	// The gitignore carve-out is checked even after the fixture cases have
	// already failed, so its own failure is reported rather than swallowed.
	if !strings.Contains(stderr, "ignored build/ still demanded a README row") {
		t.Fatalf("stderr = %q, want the gitignore carve-out named", stderr)
	}
	// No OK line beside it, or a CI log would carry both verdicts.
	if strings.Contains(stdout, "selftest OK") {
		t.Fatalf("stdout = %q, want no OK alongside the failure", stdout)
	}
}

// Every failure is collected before anything is printed, rather than the run
// stopping at the first one. A reader fixing a blinded detector wants the
// whole list, since one cause usually breaks several cases at once.
func TestAFailingSelfTestReportsEveryCaseThatBroke(t *testing.T) {
	withRowNamePattern(t, regexp.MustCompile(`^never-written-in-a-readme-row$`))

	_, _, stderr := ranSelfTest(t)

	for _, want := range []string{
		"in-sync stays quiet",   // documented rows vanished, so drift fires
		"stale doc entry fires", // the stale name can no longer be read
		"ignored build/ still demanded a README row",
	} {
		if !strings.Contains(stderr, want) {
			t.Errorf("stderr = %q, want %q reported too", stderr, want)
		}
	}
}
