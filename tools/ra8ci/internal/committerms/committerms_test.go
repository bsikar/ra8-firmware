// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package committerms

import (
	"bytes"
	"context"
	"strings"
	"testing"
)

func TestFindViolationsTermsAndUnicodeBoundaries(t *testing.T) {
	text := strings.Join([]string{
		"masters", "mastered", "mastering", "mastership", "slaves", "slaved",
		"MOSI", "MISO", "Slave Select", "Slave-Select", "Slave_Select",
		"masterful", "enslaved", "Mosi", "éMOSI", "MOSIé",
	}, "\n")
	got := FindViolations(text)
	if len(got) != 11 {
		t.Fatalf("found %d violations, want 11: %v", len(got), got)
	}
	if !strings.Contains(got[0], "line 1: master --") || !strings.Contains(got[6], "line 7: MOSI --") ||
		!strings.Contains(got[8], "line 9: slave --") || !strings.Contains(got[10], "line 11: Slave Select --") {
		t.Fatalf("unexpected line/pattern selection: %v", got)
	}
}

func TestPythonSplitlinesSeparatorsRemainLineBoundaries(t *testing.T) {
	for _, separator := range []rune{0x0b, 0x0c, 0x1c, 0x1d, 0x1e, 0x85, 0x2028, 0x2029} {
		got := FindViolations("clean" + string(separator) + "MOSI")
		if len(got) != 1 || !strings.Contains(got[0], "line 2:") {
			t.Errorf("separator %U: violations=%v", separator, got)
		}
	}
}

func TestLegacyOKIsParagraphScopedAcrossWrappedLines(t *testing.T) {
	quiet := FindViolations("header\r\n\r\nmaster/slave MOSI/MISO\r\nLEGACY-OK\u3000: upstream quote\r\n")
	if len(quiet) != 0 {
		t.Fatalf("same-paragraph opt-out failed: %v", quiet)
	}
	crossParagraph := FindViolations("MOSI\n\nLEGACY-OK: unrelated paragraph\n")
	if len(crossParagraph) != 1 || !strings.Contains(crossParagraph[0], "line 1:") {
		t.Fatalf("cross-paragraph opt-out leaked: %v", crossParagraph)
	}
}

func TestRunReadsStdinAndReturnsDetectorStatus(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), nil, strings.NewReader("clean commit subject\n"), &stdout, &stderr); code != 0 {
		t.Fatalf("clean exit=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	if !strings.Contains(stdout.String(), "[PASS]") || stderr.Len() != 0 {
		t.Fatalf("clean output stdout=%q stderr=%q", stdout.String(), stderr.String())
	}
	stdout.Reset()
	if code := Run(context.Background(), nil, strings.NewReader("rename MOSI pin\n"), &stdout, &stderr); code != 1 {
		t.Fatalf("violation exit=%d stdout=%q", code, stdout.String())
	}
	if !strings.Contains(stdout.String(), "line 1: MOSI -- use COPI") {
		t.Fatalf("violation output = %q", stdout.String())
	}
}

func TestSelftestAndArgumentValidation(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), []string{"--selftest"}, strings.NewReader("ignored"), &stdout, &stderr); code != 0 {
		t.Fatalf("selftest exit=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	stdout.Reset()
	stderr.Reset()
	if code := Run(context.Background(), []string{"--unknown"}, strings.NewReader(""), &stdout, &stderr); code != 2 {
		t.Fatalf("bad argument exit=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}
