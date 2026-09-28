//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"strings"
	"testing"
)

// Every field the walk positions after the comm is read as a number, and a
// line that does not carry one is refused rather than parsed into a process
// identity the teardown would then signal.
func TestParseProcStatRefusesAFieldThatIsNotANumber(t *testing.T) {
	filler := strings.Repeat("0 ", 12)
	for name, line := range map[string]string{
		"parent is not a number":     "9 (sh) S x 5 5 0 -1 0 " + filler + "7 0 0",
		"group is not a number":      "9 (sh) S 5 x 5 0 -1 0 " + filler + "7 0 0",
		"start time is not a number": "9 (sh) S 5 5 5 0 -1 0 " + strings.Repeat("0 ", 12) + "x 0 0",
		"start time is negative":     "9 (sh) S 5 5 5 0 -1 0 " + strings.Repeat("0 ", 12) + "-7 0 0",
		"group is negative":          "9 (sh) S 5 -5 5 0 -1 0 " + filler + "7 0 0",
		"nothing after the comm":     "9 (sh)",
		"one byte after the comm":    "9 (sh) ",
	} {
		if got, ok := parseProcStat(9, []byte(line)); ok {
			t.Errorf("%s: accepted %q as %+v", name, line, got)
		}
	}
}

// The comm field is unquoted and may hold anything, so the fields are
// positioned from the LAST ')' in the line. A process whose own name carries a
// closing parenthesis must still be read correctly, or the teardown signals
// the wrong group.
func TestParseProcStatPositionsFromTheLastParenthesisEvenWhenCommIsHostile(t *testing.T) {
	line := "31 ((()) sh )) S 30 28 28 0 -1 0 " + strings.Repeat("0 ", 12) + "515 0 0"
	got, ok := parseProcStat(31, []byte(line))
	if !ok {
		t.Fatalf("refused %q", line)
	}
	want := treeProcess{PID: 31, PPID: 30, PGID: 28, StartTicks: 515}
	if got != want {
		t.Fatalf("parsed = %+v, want %+v", got, want)
	}
}

// A negative pid is refused from the caller's side too: the walk reads the
// directory name, and a value it could not have come from is not a process.
func TestParseProcStatRefusesANegativeProcessID(t *testing.T) {
	line := "-3 (sh) S 1 1 1 0 -1 0 " + strings.Repeat("0 ", 12) + "7 0 0"
	if _, ok := parseProcStat(-3, []byte(line)); ok {
		t.Fatal("a negative pid was accepted")
	}
}
