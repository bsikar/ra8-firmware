// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// countedStep builds a reviewed tool step dispatching program with args.
func countedStep(program string, args ...string) Step {
	return Step{Name: "gate", Program: program, Args: args}
}

func TestAToolTakingOneTargetRefusesTwo(t *testing.T) {
	err := ValidateStepDispatch(countedStep("ra8ci:ascii", "src/ra8_batt.c", "src/ra8_ui.c"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("two targets handed to a tool taking one were admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "exactly one") {
		t.Fatalf("refusal does not say the ceiling: %v", err)
	}
	if !strings.Contains(err.Error(), "src/ra8_batt.c") || !strings.Contains(err.Error(), "src/ra8_ui.c") {
		t.Fatalf("refusal does not name the targets that collide: %v", err)
	}
}

func TestAToolTakingOneTargetStillTakesOne(t *testing.T) {
	if err := ValidateStepDispatch(countedStep("ra8ci:ascii", "src/ra8_batt.c")); err != nil {
		t.Fatalf("one target was refused: %v", err)
	}
}

func TestTheCheckoutOptionStillAccompaniesItsOneTarget(t *testing.T) {
	if err := ValidateStepDispatch(countedStep("ra8ci:ascii", "--checkout", "src/ra8_batt.c")); err != nil {
		t.Fatalf("--checkout beside its one target was refused: %v", err)
	}
	if err := ValidateStepDispatch(countedStep("ra8ci:ascii", "--check", "--checkout", "src/ra8_batt.c")); err != nil {
		t.Fatalf("a reporting run over one target was refused: %v", err)
	}
}

func TestTheCheckoutOptionDoesNotExcuseASecondTarget(t *testing.T) {
	err := ValidateStepDispatch(countedStep("ra8ci:ascii", "--checkout", "src/ra8_batt.c", "src/ra8_ui.c"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("an option before two targets hid the count: %v", err)
	}
}

func TestTheTargetCountIsReadInEitherDashSpelling(t *testing.T) {
	if err := ValidateStepDispatch(countedStep("ra8ci:ascii", "-checkout", "src/ra8_batt.c")); err != nil {
		t.Fatalf("a single-dash option was counted as a target: %v", err)
	}
	if err := ValidateStepDispatch(countedStep("ra8ci:ascii", "--checkout=1", "src/ra8_batt.c")); err != nil {
		t.Fatalf("an --option=value form was counted as a target: %v", err)
	}
}

func TestAToolScanningEveryTargetStillTakesMany(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if !ToolReadsFileArguments(program) {
			continue
		}
		if _, stated := ToolTargetCeiling(program); stated {
			continue
		}
		step := countedStep(program, "src/ra8_batt.c", "src/ra8_ui.c", "src/ra8_cache.c")
		if err := ValidateStepDispatch(step); err != nil {
			t.Fatalf("%s refused a list of targets it scans: %v", program, err)
		}
	}
}

func TestEveryToolThatReadsFilesSaysHowManyItTakes(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		ceiling, stated := ToolTargetCeiling(program)
		if stated && !ToolReadsFileArguments(program) {
			t.Fatalf("%s states a target ceiling but reads no file arguments", program)
		}
		if stated && ceiling < 1 {
			t.Fatalf("%s states a ceiling of %d, which would refuse every dispatch", program, ceiling)
		}
	}
	for _, program := range toolsStatingATargetCeiling() {
		if !isReviewedProgram(program) {
			t.Fatalf("%s states a target ceiling and is not a dispatched tool", program)
		}
	}
}

func TestTheWholeTreeScanIsRefusedByItsOwnDoor(t *testing.T) {
	err := ValidateStepDispatch(countedStep("ra8ci:ascii", "--all", "src/ra8_batt.c"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a path beside --all was admitted: %v", err)
	}
	if strings.Contains(err.Error(), "exactly one") {
		t.Fatalf("the scope contradiction was reported as a count: %v", err)
	}
}

func TestTheWholeTreeScanAloneIsStillAdmitted(t *testing.T) {
	if err := ValidateStepDispatch(countedStep("ra8ci:ascii", "--all")); err != nil {
		t.Fatalf("a whole-tree scan was refused: %v", err)
	}
}

func TestTheSelfTestIsUntouchedByTheTargetCount(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if err := ValidateStepDispatch(countedStep(program, "--selftest")); err != nil {
			t.Fatalf("%s refused its own self test: %v", program, err)
		}
	}
}

func TestTheShippedCatalogNamesNoExtraTarget(t *testing.T) {
	source, err := Load()
	if err != nil {
		t.Fatalf("the shipped catalog no longer loads: %v", err)
	}
	for _, name := range source.Names() {
		task, found := source.Task(name)
		if !found {
			t.Fatalf("the shipped catalog names %q and does not hold it", name)
		}
		for _, step := range task.Steps {
			if err := checkTheTargetCountIsOneTheToolTakes(step, step.Program); err != nil {
				t.Fatalf("shipped task %q: %v", task.Name, err)
			}
		}
	}
}

// isReviewedProgram reports whether a program is on the dispatch list.
func isReviewedProgram(program string) bool {
	for _, candidate := range ReviewedToolPrograms() {
		if candidate == program {
			return true
		}
	}
	return false
}
