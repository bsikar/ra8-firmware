// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package committerms

import (
	"bytes"
	"context"
	"strings"
	"testing"
)

// excused reports whether a one-paragraph message carrying a banned term is
// silenced by the annotation under test.
func excused(paragraph string) bool {
	return len(FindViolations(paragraph)) == 0
}

func TestAnOptOutWithNoReasonDoesNotSilenceTheParagraph(t *testing.T) {
	if excused("rework the MOSI pin mux\nLEGACY-OK:\n") {
		t.Fatal("a bare LEGACY-OK: silenced the paragraph")
	}
}

func TestAnOptOutWhoseReasonIsOnlyWhitespaceDoesNotSilence(t *testing.T) {
	if excused("rework the MOSI pin mux\nLEGACY-OK:   \t \n") {
		t.Fatal("a whitespace-only reason silenced the paragraph")
	}
}

func TestAReasonPromisedOnTheNextLineDoesNotSilence(t *testing.T) {
	if excused("rework the MOSI pin mux\nLEGACY-OK:\nupstream pin label, quoted verbatim\n") {
		t.Fatal("a reason on a later line silenced the paragraph")
	}
}

func TestAnOptOutStatingItsReasonStillSilences(t *testing.T) {
	if !excused("rework the MOSI pin mux\nLEGACY-OK: upstream pin label, quoted verbatim\n") {
		t.Fatal("a stated reason failed to silence its paragraph")
	}
}

func TestTheAnnotationStaysCaseInsensitive(t *testing.T) {
	if !excused("rework the MOSI pin mux\nlegacy-ok: upstream pin label\n") {
		t.Fatal("a lower-case annotation stopped working")
	}
}

func TestWhitespaceMayStillSitBeforeTheColon(t *testing.T) {
	if !excused("rework the MOSI pin mux\nLEGACY-OK\u3000: upstream pin label\n") {
		t.Fatal("wrapped whitespace before the colon stopped working")
	}
}

func TestAnAnnotationWeldedIntoALongerTokenIsNotAnOptOut(t *testing.T) {
	for _, line := range []string{
		"NOT-LEGACY-OK: this names the annotation, it does not claim one",
		"xLEGACY-OK: still part of a longer word",
		"2LEGACY-OK: still part of a longer word",
		"_LEGACY-OK: still part of a longer word",
	} {
		if excused("rework the MOSI pin mux\n" + line + "\n") {
			t.Errorf("%q was read as an opt-out", line)
		}
	}
}

func TestAnAnnotationAfterOrdinaryPunctuationStillCounts(t *testing.T) {
	for _, line := range []string{
		"(LEGACY-OK: upstream pin label)",
		"note, LEGACY-OK: upstream pin label",
		"LEGACY-OK: upstream pin label",
	} {
		if !excused("rework the MOSI pin mux\n" + line + "\n") {
			t.Errorf("%q was not read as an opt-out", line)
		}
	}
}

func TestTheFirstCompleteAnnotationOnALineWins(t *testing.T) {
	if !excused("rework the MOSI pin mux\nLEGACY-OK: and a second LEGACY-OK: with a reason\n") {
		t.Fatal("a complete annotation later on the line was not found")
	}
	if excused("rework the MOSI pin mux\nNOT-LEGACY-OK: LEGACY-OK:\n") {
		t.Fatal("two incomplete annotations on one line silenced the paragraph")
	}
}

func TestARunReportsTheViolationABareOptOutNoLongerHides(t *testing.T) {
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), nil, strings.NewReader("rename MOSI pin\nLEGACY-OK:\n"), &stdout, &stderr)
	if code != 1 || !strings.Contains(stdout.String(), "line 1: MOSI -- use COPI") {
		t.Fatalf("exit=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}
