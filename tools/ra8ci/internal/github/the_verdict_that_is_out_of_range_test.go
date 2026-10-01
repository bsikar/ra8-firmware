// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"strings"
	"testing"
)

// Every verdict names itself, and a value outside the four says so rather
// than borrowing a word. A shadow report is read by an operator deciding
// whether to hand a required check over, so a number that fell outside the
// grading must never print as "agreed" or as an empty string: the report has
// to show that the plane graded something it does not understand.
func TestEveryShadowVerdictNamesItself(t *testing.T) {
	for verdict, want := range map[ShadowVerdict]string{
		ShadowIndeterminate: "indeterminate",
		ShadowAgreed:        "agreed",
		ShadowDivergent:     "divergent",
		ShadowConflicting:   "conflicting",
	} {
		if got := verdict.String(); got != want {
			t.Fatalf("ShadowVerdict(%d).String() = %q, want %q", int(verdict), got, want)
		}
	}

	for _, verdict := range []ShadowVerdict{-1, 4, 99} {
		got := verdict.String()
		if !strings.HasPrefix(got, "ShadowVerdict(") || !strings.Contains(got, itoaForTest(int(verdict))) {
			t.Fatalf("ShadowVerdict(%d).String() = %q, want the value named", int(verdict), got)
		}
		for _, word := range []string{"indeterminate", "agreed", "divergent", "conflicting"} {
			if strings.Contains(got, word) {
				t.Fatalf("ShadowVerdict(%d) borrowed the word %q: %q", int(verdict), word, got)
			}
		}
	}
}
