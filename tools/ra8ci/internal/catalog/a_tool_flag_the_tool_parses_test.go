// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func optionStep(program string, args ...string) Step {
	return Step{Name: "gate", Program: program, Args: args}
}

func TestAToolStepPassingOptionsItsToolParsesIsAdmitted(t *testing.T) {
	step := optionStep("ra8ci:ascii", "--check", "--all")
	if err := ValidateStepDispatch(step); err != nil {
		t.Fatalf("options the tool parses must be admitted: %v", err)
	}
}

func TestAToolStepPassingAnOptionTheToolDoesNotParseIsRefused(t *testing.T) {
	step := optionStep("ra8ci:legacy-make", "--all")
	err := ValidateStepDispatch(step)
	if err == nil {
		t.Fatal("an option the tool answers with usage must be refused at review")
	}
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the refusal must be an invalid-catalog error: %v", err)
	}
	if !strings.Contains(err.Error(), "--selftest") {
		t.Fatalf("the refusal must name what the tool does parse: %v", err)
	}
}

func TestAMisspelledSelftestIsRefusedAtReview(t *testing.T) {
	if err := ValidateStepDispatch(optionStep("ra8ci:no-null", "--sefltest")); err == nil {
		t.Fatal("a typo in an option name must not reach a runner")
	}
}

func TestAnOptionOneToolParsesIsStillRefusedOnAnother(t *testing.T) {
	if err := ValidateStepDispatch(optionStep("ra8ci:runner-clock", "--repo", "bsikar/ra8-firmware")); err != nil {
		t.Fatalf("runner-clock parses --repo: %v", err)
	}
	if err := ValidateStepDispatch(optionStep("ra8ci:since", "--repo", "bsikar/ra8-firmware")); err == nil {
		t.Fatal("since does not parse --repo and must be refused")
	}
}

func TestASingleDashOptionIsReadAsTheOptionItNames(t *testing.T) {
	if err := ValidateStepDispatch(optionStep("ra8ci:ascii", "-selftest")); err != nil {
		t.Fatalf("the flag package reads -selftest and so must this door: %v", err)
	}
	if err := ValidateStepDispatch(optionStep("ra8ci:ascii", "-sefltest")); err == nil {
		t.Fatal("a single-dash typo must be refused the same as a double-dash one")
	}
}

func TestAValuedOptionIsReadWithoutItsValue(t *testing.T) {
	if err := ValidateStepDispatch(optionStep("ra8ci:runner-clock", "--runs=40")); err != nil {
		t.Fatalf("--option=value names the option: %v", err)
	}
	if err := ValidateStepDispatch(optionStep("ra8ci:runner-clock", "--run=40")); err == nil {
		t.Fatal("the name before the equals sign is still judged")
	}
}

func TestADashNamingNoOptionIsRefused(t *testing.T) {
	for _, arg := range []string{"-", "--"} {
		if err := ValidateStepDispatch(optionStep("ra8ci:ascii", arg)); err == nil {
			t.Fatalf("%q names no option and must be refused", arg)
		}
	}
}

func TestAFileArgumentIsNotJudgedAsAnOption(t *testing.T) {
	step := optionStep("ra8ci:assert-casts", "drivers/ra8_batt.c")
	if err := ValidateStepDispatch(step); err != nil {
		t.Fatalf("a path is the checkout's question, not review's: %v", err)
	}
}

func TestATaskCarriesTheToolOptionDoorToEveryStep(t *testing.T) {
	task := Task{Name: "gates", Steps: []Step{
		optionStep("ra8ci:legacy-make", "--selftest"),
		optionStep("ra8ci:legacy-make", "--checkout"),
	}}
	if err := ValidateTaskDispatch(task); err == nil {
		t.Fatal("a later step naming an unparsed option must refuse the task")
	}
}

func TestTheToolOptionDoorSaysNothingAboutShellSteps(t *testing.T) {
	step := Step{Name: "gate", Program: DispatchShell, Args: []string{"scripts/ci" + ScriptPathSuffix, "--gate", "--sefltest"}}
	if err := ValidateStepDispatch(step); err != nil {
		t.Fatalf("a reviewed script owns its own options: %v", err)
	}
}

func TestWaveReferencesRefusesAnOptionItWouldSilentlyIgnore(t *testing.T) {
	if err := ValidateStepDispatch(optionStep("ra8ci:wave-references", "--all")); err == nil {
		t.Fatal("an ignored option makes the verdict something review did not approve")
	}
}

func TestEveryReviewedToolStatesTheOptionsItParses(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if _, known := ReviewedToolFlags(program); !known {
			t.Errorf("%s is dispatched but states no reviewed options", program)
		}
	}
	for program := range reviewedToolFlags {
		if !IsReviewedToolProgram(program) {
			t.Errorf("%s states options but no runner dispatches it", program)
		}
	}
}

func TestEveryReviewedToolParsesItsSelftest(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if err := ValidateStepDispatch(optionStep(program, "--selftest")); err != nil {
			t.Errorf("%s must admit its own selftest step: %v", program, err)
		}
	}
}

func TestTheEmbeddedCatalogPassesTheToolOptionDoor(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must be admitted: %v", err)
	}
}
