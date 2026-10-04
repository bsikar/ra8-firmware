// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package gnuattribute

import (
	"bytes"
	"regexp"
	"strings"
	"testing"
)

// Every verdict this gate reaches is read through one pattern, so a
// pattern that stops recognizing the annotation reads a tree of weak
// symbols as clean. The self-test exists to catch exactly that, and its
// own failure path had never run: the count was never incremented and the
// summary was never printed, which left the alarm untested.
func withAttrPattern(t *testing.T, replacement *regexp.Regexp) {
	t.Helper()
	original := attr
	attr = replacement
	t.Cleanup(func() { attr = original })
}

// A pattern matching nothing fails only the two detection cases: the five
// exemption cases still expect zero findings and get zero, so they pass
// for the wrong reason. That asymmetry is the point, and the count has to
// name it as two failures rather than a blanket refusal.
func TestASelfTestWithABlindAttributeReaderCountsItsDetectionFailures(t *testing.T) {
	// A pattern that can never appear in C, rather than an empty-width one:
	// scan splits on newlines, so a zero-width pattern matches the trailing
	// empty line of every case and looks like a detection.
	withAttrPattern(t, regexp.MustCompile(`__attribute_never_written_this_way__\(\(`))

	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	if code := selfTest(stdout, stderr); code != 1 {
		t.Fatalf("a reader blind to __attribute__(( answered %d, not 1", code)
	}
	if !strings.Contains(stderr.String(), "2 failure(s)") {
		t.Fatalf("the two detection cases were not counted: %q", stderr.String())
	}
	for _, name := range []string{"detect weak", "detect dunder packed"} {
		if !strings.Contains(stdout.String(), "[FAIL] "+name) {
			t.Fatalf("%q was not named as failing: %q", name, stdout.String())
		}
	}
	if strings.Contains(stdout.String(), "selftest: pass") {
		t.Fatalf("a failed self-test still reported passing: %q", stdout.String())
	}
}

// A pattern that matches every line is the other way to go blind, and it
// is worse: the exemptions become findings, so the gate would refuse a
// tree that is entirely correct. All seven cases have to be counted.
func TestASelfTestWithAnIndiscriminateReaderFailsEveryCase(t *testing.T) {
	withAttrPattern(t, regexp.MustCompile(`^`))

	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	if code := selfTest(stdout, stderr); code != 1 {
		t.Fatalf("a reader matching every line answered %d, not 1", code)
	}
	if !strings.Contains(stderr.String(), "7 failure(s)") {
		t.Fatalf("every case should have failed: %q", stderr.String())
	}
}

// The control: the shipped pattern passes all seven and says so, which is
// what makes the two failures above the reader being swapped rather than
// the compiled-in cases having rotted.
func TestTheAttributeReaderAsShippedPassesItsSelfTest(t *testing.T) {
	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	if code := selfTest(stdout, stderr); code != 0 {
		t.Fatalf("the shipped reader failed its own self-test: %d %q", code, stderr.String())
	}
	if !strings.Contains(stdout.String(), "selftest: pass") {
		t.Fatalf("a passing self-test did not say so: %q", stdout.String())
	}
	if stderr.Len() != 0 {
		t.Fatalf("a passing self-test wrote to stderr: %q", stderr.String())
	}
}

// A finding carries the offending line back to the operator, and a
// generated or minified source can put a whole translation unit on one
// line. The report trims at 100 bytes so one line cannot bury the rest,
// and the trim had never run.
func TestALongOffendingLineIsReportedTrimmed(t *testing.T) {
	padding := strings.Repeat("y", 200)
	source := "int x __attribute__((packed)); int " + padding + ";\n"

	found := scan(source)
	if len(found) != 1 {
		t.Fatalf("expected the packed attribute to be found once, got %d", len(found))
	}
	if len(found[0].snippet) != 100 {
		t.Fatalf("a %d-byte line was reported as %d bytes, not trimmed to 100", len(source), len(found[0].snippet))
	}
	if !strings.HasPrefix(found[0].snippet, "int x __attribute__((packed));") {
		t.Fatalf("the trim dropped the head of the line: %q", found[0].snippet)
	}
	if found[0].line != 1 {
		t.Fatalf("the finding was reported at line %d, not 1", found[0].line)
	}
}

// A short line is handed back whole, so the trim above is the bound doing
// its work rather than the report always truncating.
func TestAShortOffendingLineIsReportedWhole(t *testing.T) {
	found := scan("  int x __attribute__((packed));  \n")
	if len(found) != 1 {
		t.Fatalf("expected one finding, got %d", len(found))
	}
	if found[0].snippet != "int x __attribute__((packed));" {
		t.Fatalf("the line was not handed back whole and trimmed of space: %q", found[0].snippet)
	}
}

// An absent root really is skipped, so discovery can run against a partial
// checkout without treating missing top-level directories as an error.
func TestAnAbsentRootIsSkipped(t *testing.T) {
	files, err := discover(t.TempDir())
	if err != nil {
		t.Fatalf("an empty checkout was refused: %v", err)
	}
	if len(files) != 0 {
		t.Fatalf("an empty checkout yielded %d files", len(files))
	}
}
