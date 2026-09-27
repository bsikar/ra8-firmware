// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import (
	"errors"
	"strings"
	"testing"
)

// The shape the rule exists for: the shell ends on the 30s default and this
// reader used to end with 180.
func TestAnAssignmentTheShellWouldNotMakeIsRefusedNotRead(t *testing.T) {
	for _, body := range []string{
		"HIL_TIMEOUT_S =180\n",
		"HIL_TIMEOUT_S\t=180\n",
		"HIL_TIMEOUT_S= 180\n",
		"HIL_TIMEOUT_S =\t180\n",
		"HIL_TIMEOUT_S  =  180\n",
		"HIL_MODE=uart_scrape\nHIL_TIMEOUT_S =180\n",
	} {
		t.Run(body, func(t *testing.T) {
			seconds, found, err := readDeclaration(t, body)
			if !errors.Is(err, ErrUnreadableDeclaration) {
				t.Fatalf("err = %v, want ErrUnreadableDeclaration", err)
			}
			if found || seconds != 0 {
				t.Fatalf("seconds=%d found=%v; a line the shell does not assign is not a declaration", seconds, found)
			}
		})
	}
}

// Separation: the same value written the way the shell reads it is still the
// declaration, including under the indentation a sourced file may carry.
func TestTheAssignmentTheShellDoesMakeIsStillRead(t *testing.T) {
	for _, body := range []string{
		"HIL_TIMEOUT_S=90\n",
		"  HIL_TIMEOUT_S=90\n",
		"\tHIL_TIMEOUT_S=90\n",
		"HIL_TIMEOUT_S=90   \n",
		"# the observe step needs a minute and a half\nHIL_MODE=uart_scrape\nHIL_TIMEOUT_S=90\n",
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
func TestTheRefusalNamesTheLineItWouldNotAssign(t *testing.T) {
	_, _, err := readDeclaration(t, "HIL_TIMEOUT_S = 180\n")
	if err == nil {
		t.Fatal("want a refusal")
	}
	if want := `"HIL_TIMEOUT_S = 180"`; !contains(err.Error(), want) {
		t.Fatalf("error %q does not name the line it would not assign", err)
	}
	if !contains(err.Error(), "hil.conf") {
		t.Fatalf("error %q does not name the file", err)
	}
	if errors.Is(err, ErrUnresolvableConfig) {
		t.Fatalf("err = %v, want a declaration refusal rather than a resolution refusal", err)
	}
}

// The boundary the other doors draw: the rule is asked only about this one
// variable, so another name written with spaces is still nobody's business.
func TestAnotherVariableWrittenWithSpacesIsStillSkippedInSilence(t *testing.T) {
	for _, body := range []string{
		"HIL_MODE = uart_scrape\n",
		"RA8_HIL_TIMEOUT_S = 180\n",
		"HIL_TIMEOUT_SEC = 180\n",
		"HIL_TIMEOUT_SECONDS= 180\n",
		"# HIL_TIMEOUT_S = 180\n",
	} {
		t.Run(body, func(t *testing.T) {
			seconds, found, err := readDeclaration(t, body)
			if err != nil || found || seconds != 0 {
				t.Fatalf("seconds=%d found=%v err=%v; another variable is not this one", seconds, found, err)
			}
		})
	}
}

// An unread spelling is reported as an unread spelling. Two refusals over one
// line would name the wrong reason, and the operator reads the reason.
func TestAnUnreadSpellingIsStillReportedAsTheSpelling(t *testing.T) {
	for _, body := range []string{
		"export HIL_TIMEOUT_S =180\n",
		"declare -i HIL_TIMEOUT_S = 180\n",
	} {
		t.Run(body, func(t *testing.T) {
			_, _, err := readDeclaration(t, body)
			if !errors.Is(err, ErrUnreadableDeclaration) {
				t.Fatalf("err = %v, want ErrUnreadableDeclaration", err)
			}
			if !contains(err.Error(), "declares HIL_TIMEOUT_S as") {
				t.Fatalf("error %q reports the spacing where it should report the spelling", err)
			}
		})
	}
}

// A line carrying no "=" belongs to the value door, and it keeps its own
// reason too.
func TestALineWithNoEqualsStillBelongsToTheValueDoor(t *testing.T) {
	_, _, err := readDeclaration(t, "unset HIL_TIMEOUT_S\n")
	if !errors.Is(err, ErrUnreadableDeclaration) {
		t.Fatalf("err = %v, want ErrUnreadableDeclaration", err)
	}
	if !contains(err.Error(), "assigning nothing") {
		t.Fatalf("error %q does not report the statement that assigns nothing", err)
	}
}

// Every refusal in this reader fails closed, and this one joins them: nothing
// about it may turn into a readable answer further down the file.
func TestARefusedAssignmentEndsTheRead(t *testing.T) {
	seconds, found, err := readDeclaration(t, "HIL_TIMEOUT_S =180\nHIL_TIMEOUT_S=90\n")
	if !errors.Is(err, ErrUnreadableDeclaration) || found || seconds != 0 {
		t.Fatalf("seconds=%d found=%v err=%v; the refusal must not be overtaken by a later line", seconds, found, err)
	}
}

func TestShellAssignsHereReadsBothSidesOfTheEquals(t *testing.T) {
	tests := []struct {
		key   string
		value string
		want  bool
	}{
		{"HIL_TIMEOUT_S", "180", true},
		{"HIL_TIMEOUT_S", "", true},
		{"HIL_TIMEOUT_S", "180 ", true},
		{"HIL_TIMEOUT_S", "180 # three minutes", true},
		{"HIL_TIMEOUT_S ", "180", false},
		{"HIL_TIMEOUT_S\t", "180", false},
		{"HIL_TIMEOUT_S  ", "180", false},
		{"HIL_TIMEOUT_S", " 180", false},
		{"HIL_TIMEOUT_S", "\t180", false},
		{"HIL_TIMEOUT_S ", " 180", false},
		{"", "180", false},
		{" ", "180", false},
	}
	for _, test := range tests {
		t.Run(test.key+"="+test.value, func(t *testing.T) {
			if got := shellAssignsHere(test.key, test.value); got != test.want {
				t.Fatalf("shellAssignsHere(%q, %q) = %t, want %t", test.key, test.value, got, test.want)
			}
		})
	}
}

// A sweep of the two positions the shell reads: only the pair that writes
// nothing at either of them is an assignment.
func TestOnlyNothingEitherSideOfTheEqualsIsAnAssignment(t *testing.T) {
	spaces := []string{"", " ", "\t", "  ", " \t "}
	for _, before := range spaces {
		for _, after := range spaces {
			line := "HIL_TIMEOUT_S" + before + "=" + after + "180"
			key, value, _ := strings.Cut(line, "=")
			wantAssignment := before == "" && after == ""
			if got := shellAssignsHere(key, value); got != wantAssignment {
				t.Fatalf("shellAssignsHere over %q = %t, want %t", line, got, wantAssignment)
			}
			seconds, found, err := readDeclaration(t, line+"\n")
			if wantAssignment {
				if err != nil || !found || seconds != 180 {
					t.Fatalf("%q: seconds=%d found=%v err=%v", line, seconds, found, err)
				}
				continue
			}
			if !errors.Is(err, ErrUnreadableDeclaration) || found || seconds != 0 {
				t.Fatalf("%q: seconds=%d found=%v err=%v, want ErrUnreadableDeclaration", line, seconds, found, err)
			}
		}
	}
}

// The value the shell would have used when it refuses to assign is the
// default, never the number written on the line, so the refusal may not be
// mistaken for a reading of that number.
func TestTheRefusedNumberIsNotHandedBack(t *testing.T) {
	seconds, found, err := readDeclaration(t, "HIL_TIMEOUT_S = 3600\n")
	if err == nil {
		t.Fatal("want a refusal")
	}
	if found || seconds != 0 {
		t.Fatalf("seconds=%d found=%v; the refused number must not be handed back", seconds, found)
	}
	decision, chooseErr := Choose(seconds, found, nil)
	if chooseErr != nil {
		t.Fatal(chooseErr)
	}
	if decision.Seconds != DefaultSeconds || decision.Source != "default" {
		t.Fatalf("decision = %+v; a refused declaration leaves the default standing", decision)
	}
}
