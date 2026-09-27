// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func companionStep(args ...string) Step {
	return Step{Name: "gate", Program: DispatchShell, Args: args}
}

func TestAContainerStepWithNoGateIsRefused(t *testing.T) {
	err := ValidateStepDispatch(companionStep("scripts/ci.sh", "--container"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("ci.sh refuses --container with no gate name, got %v", err)
	}
	if !strings.Contains(err.Error(), "--container") || !strings.Contains(err.Error(), "--gate") {
		t.Fatalf("the refusal must name both halves of the pair, got %q", err)
	}
}

func TestAContainerStepNamingAGateIsAdmitted(t *testing.T) {
	for _, args := range [][]string{
		{"scripts/ci.sh", "--gate", "format", "--container"},
		{"scripts/ci.sh", "--container", "--gate", "format"},
		{"scripts/ci.sh", "--gate=format", "--container"},
	} {
		if err := ValidateStepDispatch(companionStep(args...)); err != nil {
			t.Fatalf("%v is the supported toolchain-image path: %v", args, err)
		}
	}
}

func TestAStepNamingNeitherHalfIsAdmitted(t *testing.T) {
	for _, args := range [][]string{
		{"scripts/ci.sh"},
		{"scripts/ci.sh", "--fast"},
		{"scripts/ci.sh", "--gate=format"},
		{"scripts/ci.sh", "--native", "--rebuild"},
	} {
		// This door only, not the whole seam: the last argv here is
		// one the mode door refuses for a different reason (the native
		// branch never reads --rebuild, a_flag_the_mode_reads.go), and
		// what this test pins is that THIS door finds no companion
		// missing in any of them.
		if err := checkAScriptStepNamesEveryCompanionItNeeds(companionStep(args...)); err != nil {
			t.Fatalf("%v names no option needing a companion: %v", args, err)
		}
	}
}

func TestTheCompanionIsReadInEitherSpelling(t *testing.T) {
	bare := ValidateStepDispatch(companionStep("scripts/ci.sh", "--container", "--gate", "format"))
	equals := ValidateStepDispatch(companionStep("scripts/ci.sh", "--container", "--gate=format"))
	if bare != nil || equals != nil {
		t.Fatalf("both spellings of --gate satisfy the companion: bare %v, equals %v", bare, equals)
	}
}

func TestASilentlyIgnoredFlagIsNotRefusedHere(t *testing.T) {
	// A single-gate run IGNORES these rather than refusing them, which the
	// door comment above called a different slice with a different
	// message. That slice landed: a_flag_the_mode_reads.go refuses them
	// now, and the seam therefore does too. What stays pinned is that the
	// refusal is not THIS door's, so the pair message keeps naming the
	// pair.
	for _, flag := range []string{"--fast", "--native", "--rebuild"} {
		if err := checkAScriptStepNamesEveryCompanionItNeeds(companionStep("scripts/ci.sh", "--gate=format", flag)); err != nil {
			t.Fatalf("a single-gate run ignores %s rather than refusing it here: %v", flag, err)
		}
	}
}

func TestAnUnstatedScriptNamesNoCompanion(t *testing.T) {
	if err := ValidateStepDispatch(companionStep("scripts/checks/format_tree.sh", "--container")); err != nil {
		t.Fatalf("this door extends where a contract is stated, not everywhere: %v", err)
	}
	if _, needs := ScriptOptionCompanion("scripts/checks/format_tree.sh", "--container"); needs {
		t.Fatal("an unstated script states no companion")
	}
}

func TestScriptOptionCompanionAgreesWithTheDoor(t *testing.T) {
	companion, needs := ScriptOptionCompanion("scripts/ci.sh", "--container")
	if !needs || companion != "--gate" {
		t.Fatalf("--container needs --gate, got %q %v", companion, needs)
	}
	if _, needs := ScriptOptionCompanion("scripts/ci.sh", "--fast"); needs {
		t.Fatal("--fast stands alone")
	}
}

func TestEveryCompanionPairIsSpelledTheWayTheScriptParsesIt(t *testing.T) {
	for script, pairs := range scriptOptionsNeedingACompanion {
		stated, known := reviewedScriptOptions[script]
		if !known {
			t.Fatalf("%s states a companion rule but no argument contract; the two tables must agree", script)
		}
		for _, pair := range pairs {
			if !statesExactly(stated.valueless, pair.option) && !statesExactly(stated.takingTheNextArgument, pair.option) {
				t.Fatalf("%s: %s is not a spelling the script parses", script, pair.option)
			}
			if !statesExactly(stated.valueless, pair.companion) && !statesExactly(stated.takingTheNextArgument, pair.companion) {
				t.Fatalf("%s: %s is not a spelling the script parses", script, pair.companion)
			}
			if pair.because == "" {
				t.Fatalf("%s: %s refuses without saying why", script, pair.option)
			}
		}
	}
}

func TestTheShippedCatalogNamesEveryCompanionItNeeds(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
