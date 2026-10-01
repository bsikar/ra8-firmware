// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// Every client command reads its endpoint through this one surface, so the
// wording of its refusal is the first thing an operator setting a command up
// ever sees. A refusal that lists the whole surface every time makes them
// re-check variables that were never the problem, and one that leaves a name
// out sends them looking in the wrong place.

// fromTable answers from a fixed set, so a test states exactly what the
// process environment holds rather than mutating it.
func fromTable(values map[string]string) func(string) string {
	return func(name string) string { return values[name] }
}

// A nil lookup is the shape a caller that forgot to pass os.Getenv hands in.
// It reads as an environment with nothing set, which is a refusal naming what
// to set, rather than a panic one variable into the walk.
func TestAnAbsentLookupReadsAsAnEmptyEnvironment(t *testing.T) {
	if value := environmentValue(nil, envServerURL); value != "" {
		t.Errorf("a nil lookup answered %q", value)
	}
	missing := missingEnvironment(nil, envServerURL, envServerCA)
	if len(missing) != 2 {
		t.Fatalf("a nil lookup reported %v missing; want both", missing)
	}
	endpoint, err := resolveClientEndpoint("ra8ci sync", roleOperator, nil)
	if err == nil {
		t.Fatalf("a nil lookup resolved to %+v", endpoint)
	}
	if !strings.HasPrefix(err.Error(), "ra8ci sync: set ") {
		t.Fatalf("err=%q; want the command and what to set", err)
	}
}

// A value that is only whitespace is a mistake, not a setting. Left alone it
// would reach os.ReadFile as a path and fail later as a missing file, which
// names the wrong problem.
func TestAWhitespaceValueIsAbsentRatherThanAPath(t *testing.T) {
	for _, blank := range []string{"", " ", "\t", "\n", "  \t \n "} {
		lookup := fromTable(map[string]string{envServerCA: blank})
		if value := environmentValue(lookup, envServerCA); value != "" {
			t.Errorf("a value of %q read as %q", blank, value)
		}
	}
	// A value with whitespace around something real is kept exactly as set:
	// trimming it would quietly rewrite a path the operator typed.
	lookup := fromTable(map[string]string{envServerCA: " /etc/ra8ci/ca.pem "})
	if value := environmentValue(lookup, envServerCA); value != " /etc/ra8ci/ca.pem " {
		t.Errorf("a real value was rewritten as %q", value)
	}
}

// The missing names come back in the order they were asked for, because the
// refusal is read as a list of things to go and set.
func TestTheMissingNamesKeepTheOrderTheyWereAskedIn(t *testing.T) {
	lookup := fromTable(map[string]string{envServerCA: "/etc/ra8ci/ca.pem"})
	missing := missingEnvironment(lookup, envServerURL, envServerCA, "RA8CI_THIRD", "RA8CI_FOURTH")
	want := []string{envServerURL, "RA8CI_THIRD", "RA8CI_FOURTH"}
	if len(missing) != len(want) {
		t.Fatalf("missing=%v; want %v", missing, want)
	}
	for index, name := range want {
		if missing[index] != name {
			t.Fatalf("missing=%v; want %v", missing, want)
		}
	}
	if len(missingEnvironment(lookup)) != 0 {
		t.Error("asking about no names reported something missing")
	}
}

// Nothing missing is not an error. This is the arm a command reaches every
// time it is configured correctly, and returning a non-nil error here would
// fail every correctly configured run.
func TestNothingMissingIsNotARefusal(t *testing.T) {
	if err := environmentError("ra8ci sync", nil); err != nil {
		t.Errorf("a complete environment was refused: %v", err)
	}
	if err := environmentError("ra8ci sync", []string{}); err != nil {
		t.Errorf("an empty list was refused: %v", err)
	}
}

// One, two and three missing names are three different sentences, and each
// has to read as English rather than as a joined slice.
func TestTheRefusalNamesOnlyWhatIsUnsetAndReadsAsASentence(t *testing.T) {
	for _, testCase := range []struct {
		missing []string
		want    string
	}{
		{[]string{"RA8CI_ONE"}, "ra8ci sync: set RA8CI_ONE"},
		{[]string{"RA8CI_ONE", "RA8CI_TWO"}, "ra8ci sync: set RA8CI_ONE and RA8CI_TWO"},
		{[]string{"RA8CI_ONE", "RA8CI_TWO", "RA8CI_THREE"}, "ra8ci sync: set RA8CI_ONE, RA8CI_TWO and RA8CI_THREE"},
	} {
		err := environmentError("ra8ci sync", testCase.missing)
		if err == nil || err.Error() != testCase.want {
			t.Errorf("%d missing answered %v; want %q", len(testCase.missing), err, testCase.want)
		}
	}
}

// The whole point of naming only what is unset: a variable already set must
// not appear in the refusal, or the operator re-checks something that was
// never the problem.
func TestARefusalNeverNamesAVariableThatIsAlreadySet(t *testing.T) {
	lookup := fromTable(map[string]string{
		envServerURL:        "https://ra8ci.example:8443",
		roleOperator.keyEnv: "/etc/ra8ci/operator.key",
	})
	_, err := resolveClientEndpoint("ra8ci report", roleOperator, lookup)
	if err == nil {
		t.Fatal("a half-set environment resolved")
	}
	for _, set := range []string{envServerURL, roleOperator.keyEnv} {
		if strings.Contains(err.Error(), set) {
			t.Errorf("err=%q; names %s, which is already set", err, set)
		}
	}
	for _, unset := range []string{envServerCA, roleOperator.certEnv} {
		if !strings.Contains(err.Error(), unset) {
			t.Errorf("err=%q; does not name %s", err, unset)
		}
	}
}

// A command that needs more than the four names says so in the same pass, so
// an operator is told everything at once rather than one variable per
// attempt.
func TestACommandsExtraNamesAreAskedForInTheSamePass(t *testing.T) {
	lookup := fromTable(map[string]string{
		envServerURL:         "https://ra8ci.example:8443",
		envServerCA:          "/etc/ra8ci/ca.pem",
		roleOperator.certEnv: "/etc/ra8ci/operator.pem",
		roleOperator.keyEnv:  "/etc/ra8ci/operator.key",
	})
	_, err := resolveClientEndpoint("ra8ci board", roleOperator, lookup, envBoardID, envBoardStateFile)
	if err == nil {
		t.Fatal("a command missing its own names resolved")
	}
	want := "ra8ci board: set " + envBoardID + " and " + envBoardStateFile
	if err.Error() != want {
		t.Fatalf("err=%q; want %q", err, want)
	}

	// With those set too, the endpoint carries exactly what was read.
	lookup = fromTable(map[string]string{
		envServerURL:         "https://ra8ci.example:8443",
		envServerCA:          "/etc/ra8ci/ca.pem",
		roleOperator.certEnv: "/etc/ra8ci/operator.pem",
		roleOperator.keyEnv:  "/etc/ra8ci/operator.key",
		envBoardID:           "board-7",
		envBoardStateFile:    "/var/lib/ra8ci/board.state",
	})
	endpoint, err := resolveClientEndpoint("ra8ci board", roleOperator, lookup, envBoardID, envBoardStateFile)
	if err != nil {
		t.Fatalf("a complete environment was refused: %v", err)
	}
	if endpoint.ServerURL != "https://ra8ci.example:8443" || endpoint.CAFile != "/etc/ra8ci/ca.pem" ||
		endpoint.CertFile != "/etc/ra8ci/operator.pem" || endpoint.KeyFile != "/etc/ra8ci/operator.key" {
		t.Fatalf("the endpoint reads %+v", endpoint)
	}
}
