// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import (
	"errors"
	"strings"
	"testing"
)

func TestAStatementThatAssignsNothingIsRefusedNotSkipped(t *testing.T) {
	for _, body := range []string{
		"unset HIL_TIMEOUT_S\n",
		"unset -v HIL_TIMEOUT_S\n",
		"export HIL_TIMEOUT_S\n",
		"HIL_TIMEOUT_S\n",
		"HIL_MODE=uart_scrape\nunset HIL_TIMEOUT_S\n",
	} {
		t.Run(body, func(t *testing.T) {
			seconds, found, err := readDeclaration(t, body)
			if !errors.Is(err, ErrUnreadableDeclaration) {
				t.Fatalf("err = %v, want ErrUnreadableDeclaration", err)
			}
			if found || seconds != 0 {
				t.Fatalf("seconds=%d found=%v; a statement this reader cannot read is not an absence", seconds, found)
			}
		})
	}
}

// The shape the rule exists for: the shell ends with no declaration and this
// reader used to end with 180.
func TestAnAssignmentTheNextLineTakesBackIsNotStillTheAnswer(t *testing.T) {
	seconds, found, err := readDeclaration(t, "HIL_TIMEOUT_S=180\nunset HIL_TIMEOUT_S\n")
	if !errors.Is(err, ErrUnreadableDeclaration) {
		t.Fatalf("seconds=%d found=%v err=%v, want ErrUnreadableDeclaration", seconds, found, err)
	}
	// Separation: the same file without that line is the declaration it reads.
	seconds, found, err = readDeclaration(t, "HIL_TIMEOUT_S=180\n")
	if err != nil || !found || seconds != 180 {
		t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
	}
}

func TestALineAboutAnotherVariableIsStillSkippedInSilence(t *testing.T) {
	for _, body := range []string{
		"unset HIL_MODE\n",
		"unset -v RA8_HIL_TIMEOUT_S\n",
		"unset HIL_TIMEOUT_SEC\n",
		"HIL_TIMEOUT\n",
		"see the README about the timeout\n",
		"# unset HIL_TIMEOUT_S\n",
	} {
		t.Run(body, func(t *testing.T) {
			seconds, found, err := readDeclaration(t, body)
			if err != nil || found || seconds != 0 {
				t.Fatalf("seconds=%d found=%v err=%v; another line is not this declaration", seconds, found, err)
			}
		})
	}
}

func TestAnOrdinaryDeclarationIsUntouchedByThisRule(t *testing.T) {
	for _, body := range []string{
		"HIL_TIMEOUT_S=90\n",
		"# comment\nHIL_MODE=uart_scrape\nHIL_EXPECT=\"verdict=PASS\"\nHIL_TIMEOUT_S=90\n",
		"  HIL_TIMEOUT_S=90\n",
	} {
		t.Run(body, func(t *testing.T) {
			seconds, found, err := readDeclaration(t, body)
			if err != nil || !found || seconds != 90 {
				t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
			}
		})
	}
}

// The refusal has to name the line, because the operator's next act is to go
// and look at it.
func TestTheRefusalNamesTheStatementItCouldNotRead(t *testing.T) {
	_, _, err := readDeclaration(t, "unset -v HIL_TIMEOUT_S\n")
	if err == nil {
		t.Fatal("want a refusal")
	}
	if want := `"unset -v HIL_TIMEOUT_S"`; !contains(err.Error(), want) {
		t.Fatalf("error %q does not name the statement it could not read", err)
	}
	if !contains(err.Error(), "hil.conf") {
		t.Fatalf("error %q does not name the file", err)
	}
	if errors.Is(err, ErrUnresolvableConfig) {
		t.Fatalf("err = %v, want a declaration refusal rather than a resolution refusal", err)
	}
}

func TestLineDeclaresTheTimeoutWithoutAValueStandsTheTokenAlone(t *testing.T) {
	tests := []struct {
		line string
		want bool
	}{
		{"unset HIL_TIMEOUT_S", true},
		{"unset -v HIL_TIMEOUT_S", true},
		{"export HIL_TIMEOUT_S", true},
		{"HIL_TIMEOUT_S", true},
		{"readonly HIL_TIMEOUT_S", true},
		{"unset HIL_TIMEOUT_S # reset to the default", true},
		{"HIL_TIMEOUT_S=90", false},
		{"export HIL_TIMEOUT_S=180", false},
		{": ${HIL_TIMEOUT_S:=180}", false},
		{"HIL_TIMEOUT_S+=60", false},
		{"unset RA8_HIL_TIMEOUT_S", false},
		{"unset HIL_TIMEOUT_SEC", false},
		{"unset HIL_TIMEOUT_S2", false},
		{"unset HIL_MODE", false},
		{"HIL_TIMEOUT", false},
		{"", false},
	}
	for _, test := range tests {
		t.Run(test.line, func(t *testing.T) {
			if got := lineDeclaresTheTimeoutWithoutAValue(test.line); got != test.want {
				t.Fatalf("lineDeclaresTheTimeoutWithoutAValue(%q) = %t, want %t", test.line, got, test.want)
			}
		})
	}
}

// The two doors divide the lines between them: a line carrying an "=" belongs
// to the spelling door and a line carrying none belongs to this one, and
// neither may answer for the other.
func TestTheTwoDoorsDivideTheLinesBetweenThem(t *testing.T) {
	lines := []string{
		"unset HIL_TIMEOUT_S",
		"export HIL_TIMEOUT_S",
		"export HIL_TIMEOUT_S=180",
		"HIL_TIMEOUT_S+=60",
		"HIL_TIMEOUT_S=90",
		"unset HIL_MODE",
		"HIL_MODE=uart_scrape",
		"RA8_HIL_TIMEOUT_S=180",
		"unset RA8_HIL_TIMEOUT_S",
		": ${HIL_TIMEOUT_S:=180}",
	}
	for _, line := range lines {
		t.Run(line, func(t *testing.T) {
			mine := lineDeclaresTheTimeoutWithoutAValue(line)
			if mine && strings.Contains(line, "=") {
				t.Fatalf("%q carries an assignment and belongs to the spelling door", line)
			}
			if !strings.Contains(line, "=") && keyNamesTheTimeout(line) != mine {
				t.Fatalf("%q names the timeout but the two answers disagree", line)
			}
		})
	}
}

// Every refusal in this reader fails closed, and this one joins them: nothing
// about it may turn into a readable answer further down the file.
func TestARefusedStatementEndsTheRead(t *testing.T) {
	seconds, found, err := readDeclaration(t, "unset HIL_TIMEOUT_S\nHIL_TIMEOUT_S=90\n")
	if !errors.Is(err, ErrUnreadableDeclaration) || found || seconds != 0 {
		t.Fatalf("seconds=%d found=%v err=%v; the refusal must not be overtaken by a later line", seconds, found, err)
	}
}
