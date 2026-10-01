// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"os"
	"strings"
	"testing"
)

// Where each of these readers stops, and one file that is longer than it
// said it was.

// The two readers over a step's argv deliberately stop in different places,
// and a reviewer reading one after the other would reasonably expect them to
// agree. They must not: the as-written reader is modelling what the flag
// package does, which is to stop at the first element it cannot name, while
// the value checker judges every flag in the list wherever it sits.
func TestTheTwoArgvReadersStopInDifferentPlacesOnPurpose(t *testing.T) {
	const program = "ra8ci:runner-clock"

	// A bare dash is not a flag name, and the flag package stops there, so
	// everything after it is a positional rather than an option.
	written := toolFlagsAsWritten([]string{"--runs=5", "-", "--hours=2"}, program)
	if _, read := written["runs"]; !read {
		t.Fatal("the flag before the bare dash was not read")
	}
	if _, read := written["hours"]; read {
		t.Fatal("a flag after the bare dash was read as an option, but the parser stops there")
	}

	// The value checker does not stop. A positional in the middle of the
	// list must not hide a bad value behind it, which is the whole reason
	// it continues rather than breaking.
	step := optionStep(program, "scan", "--runs=0")
	err := checkToolFlagValuesAreOnesTheToolAccepts(step, program)
	if err == nil {
		t.Fatal("a bad flag value behind a positional went unjudged")
	}
	if !strings.Contains(err.Error(), "--runs must be at least 1") {
		t.Fatalf("the refusal does not say what is wrong: %v", err)
	}
}

// A repository is two non-empty segments. An empty one is the spelling a
// trailing slash produces, and an over-long one is the bound runner-clock's
// own pattern states, so both have to be refused before the step is reviewed
// rather than at dispatch.
func TestARepositoryValueIsRefusedForEitherSegmentBeingUnusable(t *testing.T) {
	const program = "ra8ci:runner-clock"
	for _, value := range []string{"owner/", "/repository", strings.Repeat("o", 101) + "/repository"} {
		step := optionStep(program, "--repo="+value)
		err := checkToolFlagValuesAreOnesTheToolAccepts(step, program)
		if err == nil {
			t.Fatalf("--repo=%q was accepted", value)
		}
		if !strings.Contains(err.Error(), "owner/repository") {
			t.Fatalf("--repo=%q was refused, but not for its shape: %v", value, err)
		}
	}

	// The bound itself, held on the accepting side, so the refusal above is
	// the length and not the letters.
	step := optionStep(program, "--repo="+strings.Repeat("o", 100)+"/repository")
	if err := checkToolFlagValuesAreOnesTheToolAccepts(step, program); err != nil {
		t.Fatalf("a segment exactly at the bound was refused: %v", err)
	}
}

// A file may be longer than the size it reported. The review reads with a
// limit of its own for exactly this reason, and has to refuse rather than
// hand back the truncated head as though it were the file: a manifest cut
// mid-object would otherwise be judged on the part that fitted.
func TestReadingRefusesAFileLongerThanItSaidItWas(t *testing.T) {
	const understated = "/proc/self/status"
	info, err := os.Lstat(understated)
	if err != nil || !info.Mode().IsRegular() || info.Size() != 0 {
		t.Skip("this system has no regular file that understates its own size")
	}

	raw, err := readCheckoutFile(understated, 4)
	if err == nil {
		t.Fatalf("read %d bytes from a file that claimed to be empty, want a refusal", len(raw))
	}
	if !strings.Contains(err.Error(), "grew past") {
		t.Fatalf("the refusal does not say the file outgrew the read: %v", err)
	}
	if raw != nil {
		t.Fatal("the truncated head was handed back beside the refusal")
	}
}
