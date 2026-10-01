// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// a_tool_flag_the_tool_reads_once_test.go holds the repeat rule itself, by
// calling checkNoToolFlagIsNamedTwice directly. That proves the rule decides
// correctly; it proves nothing about whether the dispatch door ever asks it.
// A checker wired to nothing passes every one of its own tests, and the step
// it was written to refuse walks into a reviewed catalog untouched.
//
// So these go through ValidateStepDispatch, the door the catalog actually
// uses, with steps whose every other door is satisfied: the flag names are
// ones runner-clock parses, the values are ones it accepts, and no target is
// named, so the repeat is the only thing left to refuse.

// The =form, where both occurrences carry their value in the element itself.
func TestTheDispatchDoorRefusesAValueFlagNamedTwice(t *testing.T) {
	err := ValidateStepDispatch(optionStep("ra8ci:runner-clock", "--runs=250", "--runs=5"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the door admitted a step that would run with 5 runs where review wrote 250: %v", err)
	}
	// The refusal has to carry both numbers out of the door, not just the
	// fact of a repeat: the whole risk here is that 250 and 5 are both
	// values runner-clock accepts, so the reviewer needs to see which one
	// the step would actually run with.
	if !strings.Contains(err.Error(), "250") || !strings.Contains(err.Error(), "5") {
		t.Fatalf("the refusal reached the caller without both spellings: %v", err)
	}
}

// The next-argument form, which is the spelling a person is most likely to
// write and the one that swallows the following element.
func TestTheDispatchDoorRefusesASeparatedValueFlagNamedTwice(t *testing.T) {
	err := ValidateStepDispatch(optionStep("ra8ci:runner-clock", "--hours", "24", "--hours", "1"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the door admitted a step that would scan 1 hour where review wrote 24: %v", err)
	}
}

// Mixed spellings are one flag, because the parser reads the name and not the
// punctuation around it. A door that compared elements literally would miss
// this pair entirely.
func TestTheDispatchDoorReadsMixedSpellingsAsOneFlag(t *testing.T) {
	for _, args := range [][]string{
		{"--runs", "250", "--runs=5"},
		{"--runs=250", "--runs", "5"},
		{"-runs=250", "--runs=5"},
	} {
		if err := ValidateStepDispatch(optionStep("ra8ci:runner-clock", args...)); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("the door admitted %v as two different flags: %v", args, err)
		}
	}
}

// And the other side of the same door, so the refusal above is the repeat
// rule answering rather than the door refusing anything it sees twice: a bare
// switch named twice discards nothing, and a valued flag named once is an
// ordinary reviewed step.
func TestTheDispatchDoorAdmitsWhatTheRepeatRuleAllows(t *testing.T) {
	for _, args := range [][]string{
		{"--ci-scan", "--ci-scan"},
		{"--runs=250"},
		{"--runs", "250", "--hours", "24"},
	} {
		if err := ValidateStepDispatch(optionStep("ra8ci:runner-clock", args...)); err != nil {
			t.Fatalf("the door refused %v, which the repeat rule allows: %v", args, err)
		}
	}
}
