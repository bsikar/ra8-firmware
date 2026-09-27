// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// pathStep builds a reviewed tool step dispatching program with args.
func pathStep(program string, args ...string) Step {
	return Step{Name: "gate", Program: program, Args: args}
}

func TestAToolThatReadsNoFilesRefusesAPath(t *testing.T) {
	err := ValidateStepDispatch(pathStep("ra8ci:legacy-make", "src/ra8_batt.c"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a path handed to a tool that reads none was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "reads no file arguments") {
		t.Fatalf("refusal does not say why: %v", err)
	}
}

func TestEveryToolThatReadsNoFilesRefusesAPath(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if ToolReadsFileArguments(program) {
			continue
		}
		if err := ValidateStepDispatch(pathStep(program, "tests")); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s admitted a file argument it does not read: %v", program, err)
		}
	}
}

func TestAToolThatReadsFilesStillTakesAPath(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if !ToolReadsFileArguments(program) {
			continue
		}
		if err := ValidateStepDispatch(pathStep(program, "src/ra8_batt.c")); err != nil {
			t.Fatalf("%s refused a file argument it reads: %v", program, err)
		}
	}
}

func TestAToolStepWithNoArgumentsIsStillAdmitted(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if err := ValidateStepDispatch(pathStep(program)); err != nil {
			t.Fatalf("%s refused a step passing nothing: %v", program, err)
		}
	}
}

func TestTheSelfTestIsStillAdmittedOnEveryTool(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if err := ValidateStepDispatch(pathStep(program, "--selftest")); err != nil {
			t.Fatalf("%s refused its own self test: %v", program, err)
		}
	}
}

func TestTheFileArgumentDoorLeavesTheOptionDoorsAlone(t *testing.T) {
	err := ValidateStepDispatch(pathStep("ra8ci:legacy-make", "--all"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("an option the tool does not parse was admitted: %v", err)
	}
	if strings.Contains(err.Error(), "reads no file arguments") {
		t.Fatalf("the file door answered for an option: %v", err)
	}
}

func TestWaveReferencesRefusesAPathItWouldIgnore(t *testing.T) {
	err := ValidateStepDispatch(pathStep("ra8ci:wave-references", "src/ra8_ui.c"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("wave-references admitted a path it silently ignores: %v", err)
	}
}

func TestTheFileArgumentDoorSaysNothingAboutTheShell(t *testing.T) {
	step := Step{Name: "gate", Program: DispatchShell, Args: []string{"scripts/ci.sh", "--gate", "ascii"}}
	if err := ValidateStepDispatch(step); err != nil {
		t.Fatalf("a reviewed shell step was refused: %v", err)
	}
}

func TestEveryReviewedToolSaysWhetherItReadsFileArguments(t *testing.T) {
	for program := range toolsReadingFileArguments {
		if !IsReviewedToolProgram(program) {
			t.Fatalf("%s reads file arguments but is not a dispatched tool", program)
		}
	}
	reading := 0
	for _, program := range ReviewedToolPrograms() {
		if ToolReadsFileArguments(program) {
			reading++
		}
	}
	if reading != len(toolsReadingFileArguments) {
		t.Fatalf("file-reading set is %d, dispatch list agrees on %d", len(toolsReadingFileArguments), reading)
	}
}

func TestTheEmbeddedCatalogIsAdmittedByTheFileArgumentDoor(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("embedded catalog refused: %v", err)
	}
	for _, name := range loaded.Names() {
		task, found := loaded.Task(name)
		if !found {
			t.Fatalf("catalog names %q but does not carry it", name)
		}
		for _, step := range task.Steps {
			program := step.Program
			if _, named := ToolProgram(program); !named {
				continue
			}
			if err := checkFileArgumentsAreOnesTheToolReads(step, program); err != nil {
				t.Fatalf("reviewed task %q step %q refused: %v", name, step.Name, err)
			}
		}
	}
}

func TestAValueTakingOptionIsNotReadAsAPath(t *testing.T) {
	steps := []Step{
		pathStep("ra8ci:runner-clock", "--repo", "bsikar/ra8-firmware"),
		pathStep("ra8ci:runner-clock", "--runs", "50"),
		pathStep("ra8ci:runner-clock", "--repo", "bsikar/ra8-firmware", "--hours", "24"),
		pathStep("ra8ci:runner-clock", "--repo=bsikar/ra8-firmware"),
	}
	for _, step := range steps {
		if err := ValidateStepDispatch(step); err != nil {
			t.Fatalf("runner-clock step %v refused: %v", step.Args, err)
		}
	}
}

func TestAPathAfterAValueTakingOptionIsStillRefused(t *testing.T) {
	step := pathStep("ra8ci:runner-clock", "--repo", "bsikar/ra8-firmware", "tests")
	if err := ValidateStepDispatch(step); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a path past a flag value was admitted: %v", err)
	}
	joined := pathStep("ra8ci:runner-clock", "--repo=bsikar/ra8-firmware", "tests")
	if err := ValidateStepDispatch(joined); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a path past a joined flag value was admitted: %v", err)
	}
}

func TestEveryValueTakingFlagIsOneItsToolParses(t *testing.T) {
	for program, flags := range valueTakingToolFlags {
		accepted, known := ReviewedToolFlags(program)
		if !known {
			t.Fatalf("%s states value-taking flags but no reviewed flags", program)
		}
		for name := range flags {
			if !statesFlag(accepted, name) {
				t.Fatalf("%s states the value-taking flag %q, which it does not parse", program, name)
			}
		}
	}
}
