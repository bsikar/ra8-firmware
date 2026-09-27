// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package waverefs

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

// excused is the question the scan asks of a line before it reports it.
func excused(line string) bool { return lineStatesAWaveOptOut(line) }

// scanLines runs the real scan over a temp tree holding one file of these lines
// and returns the lines it reported.
func scanLines(t *testing.T, lines ...string) []string {
	t.Helper()
	root := t.TempDir()
	rel := "notes.md"
	body := ""
	for _, line := range lines {
		body += line + "\n"
	}
	if err := os.WriteFile(filepath.Join(root, rel), []byte(body), 0o600); err != nil {
		t.Fatalf("write fixture: %v", err)
	}
	found, err := scan(context.Background(), root, []string{rel})
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	reported := make([]string, 0, len(found))
	for _, item := range found {
		reported = append(reported, item.text)
	}
	return reported
}

func TestAnOptOutWithAReasonExcusesTheLine(t *testing.T) {
	if !excused("see Wave 12 WAVE-OK: quoted from the vendor changelog") {
		t.Fatal("an opt-out stating its reason must excuse the line")
	}
}

func TestABareOptOutDoesNotExcuseTheLine(t *testing.T) {
	if excused("see Wave 12 WAVE-OK:") {
		t.Fatal("an opt-out promising a reason it never gives must not excuse the line")
	}
}

func TestAnOptOutFollowedOnlyByWhitespaceDoesNotExcuseTheLine(t *testing.T) {
	if excused("see Wave 12 WAVE-OK:   \t ") {
		t.Fatal("trailing whitespace is not a reason")
	}
}

func TestAnOptOutWithoutItsColonDoesNotExcuseTheLine(t *testing.T) {
	if excused("see Wave 12 WAVE-OK quoted from the vendor changelog") {
		t.Fatal("the marker alone is not the annotation")
	}
}

func TestWhitespaceBetweenTheMarkerAndItsColonStaysAllowed(t *testing.T) {
	if !excused("see Wave 12 WAVE-OK : wrapped by the formatter") {
		t.Fatal("a wrapped annotation must still excuse the line")
	}
}

func TestAMarkerWeldedToALongerTokenDoesNotExcuseTheLine(t *testing.T) {
	for _, line := range []string{
		"NOT-WAVE-OK: this line is about the annotation, Wave 12",
		"xWAVE-OK: still about it, Wave 12",
		"3WAVE-OK: still about it, Wave 12",
		"_WAVE-OK: still about it, Wave 12",
	} {
		if excused(line) {
			t.Errorf("a welded marker must not excuse the line: %q", line)
		}
	}
}

func TestAMarkerAfterOrdinaryPunctuationStillExcusesTheLine(t *testing.T) {
	for _, line := range []string{
		"see Wave 12 (WAVE-OK: quoted heading)",
		"see Wave 12 [WAVE-OK: quoted heading]",
		"# Wave 12 WAVE-OK: section title carried over",
	} {
		if !excused(line) {
			t.Errorf("punctuation before the marker must not disqualify it: %q", line)
		}
	}
}

func TestASecondMarkerOnTheLineCanCarryTheReason(t *testing.T) {
	if !excused("NOT-WAVE-OK is the wrong spelling, Wave 12 WAVE-OK: quoted heading") {
		t.Fatal("a welded first marker must not hide a real annotation later on the line")
	}
}

func TestALineWithNoMarkerIsNotExcused(t *testing.T) {
	if excused("fixed in Wave 70") {
		t.Fatal("a line with no annotation must not be excused")
	}
}

func TestScanReportsTheLinesWhoseOptOutStatesNoReason(t *testing.T) {
	reported := scanLines(t,
		"fixed in Wave 70 WAVE-OK: quoted from the vendor changelog",
		"fixed in Wave 71 WAVE-OK:",
		"fixed in Wave 72 NOT-WAVE-OK: about the annotation",
		"the sine wave is smooth",
	)
	if len(reported) != 2 {
		t.Fatalf("scan reported %d line(s), want 2: %v", len(reported), reported)
	}
	if reported[0] != "fixed in Wave 71 WAVE-OK:" {
		t.Errorf("first finding is %q", reported[0])
	}
	if reported[1] != "fixed in Wave 72 NOT-WAVE-OK: about the annotation" {
		t.Errorf("second finding is %q", reported[1])
	}
}

func TestScanStillHonoursAnAnnotationThatStatesItsReason(t *testing.T) {
	reported := scanLines(t, "fixed in Wave 70 WAVE-OK: quoted from the vendor changelog")
	if len(reported) != 0 {
		t.Fatalf("scan reported %v, want nothing", reported)
	}
}
