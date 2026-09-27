// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func repeatedStep(name, program string, args ...string) Step {
	return Step{Name: name, Program: program, Args: args}
}

func readOnlyRepeatTask(steps ...Step) Task {
	return Task{Name: "gates", Scope: "safe-local-read-only", Steps: steps}
}

func TestTwoReadOnlyStepsRunningTheSameCommandAreRefused(t *testing.T) {
	err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("format", "bash", "scripts/ci.sh", "--gate", "format"),
		repeatedStep("format-again", "bash", "scripts/ci.sh", "--gate", "format"),
	))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
}

func TestTheRepeatRefusalNamesBothStepsAndTheCommand(t *testing.T) {
	err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("format", "bash", "scripts/ci.sh", "--gate", "format"),
		repeatedStep("format-again", "bash", "scripts/ci.sh", "--gate", "format"),
	))
	if err == nil {
		t.Fatal("want a refusal")
	}
	for _, want := range []string{`"format"`, `"format-again"`, "bash scripts/ci.sh --gate format"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal %q does not name %s", err, want)
		}
	}
}

func TestTheRepeatRefusalNamesTheFirstStepThatRanTheCommand(t *testing.T) {
	err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("first", "bash", "scripts/ci.sh", "--fast"),
		repeatedStep("other", "bash", "scripts/ci.sh", "--native"),
		repeatedStep("third", "bash", "scripts/ci.sh", "--fast"),
	))
	if err == nil || !strings.Contains(err.Error(), `"first"`) || !strings.Contains(err.Error(), `"third"`) {
		t.Fatalf("want the first and the repeat named, got %v", err)
	}
	if strings.Contains(err.Error(), `"other"`) {
		t.Fatalf("refusal names an unrelated step: %v", err)
	}
}

// A writing task repeating a command is the check, rewrite, check-again shape,
// where the second run is the point rather than the mistake. Every scope but
// safe-local-read-only may write, so only that one is judged.
func TestAWritingTaskMayRunTheSameCommandTwice(t *testing.T) {
	for _, scope := range []string{"safe-local-write-working-tree", "runner", "linux-vm", "windows-vm", "hil"} {
		task := readOnlyRepeatTask(
			repeatedStep("check", "bash", "scripts/ci.sh", "--gate", "format"),
			repeatedStep("rewrite", "bash", "scripts/checks/format_tree.sh"),
			repeatedStep("check-again", "bash", "scripts/ci.sh", "--gate", "format"),
		)
		task.Scope = scope
		if err := checkNoStepRepeatsAnotherStepsCommand(task); err != nil {
			t.Fatalf("scope %q may write between the two runs: %v", scope, err)
		}
	}
}

func TestTwoStepsSharingAProgramWithDifferentArgumentsAreAdmitted(t *testing.T) {
	if err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("format", "bash", "scripts/ci.sh", "--gate", "format"),
		repeatedStep("misra", "bash", "scripts/ci.sh", "--gate", "misra"),
	)); err != nil {
		t.Fatalf("one gate over two targets is the ordinary shape: %v", err)
	}
}

func TestTwoStepsNamingTheSameOptionsInADifferentOrderAreAdmitted(t *testing.T) {
	if err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("one", "bash", "scripts/ci.sh", "--fast", "--native"),
		repeatedStep("two", "bash", "scripts/ci.sh", "--native", "--fast"),
	)); err != nil {
		t.Fatalf("a different argv is a different command: %v", err)
	}
}

func TestTwoArgumentlessStepsNamingTheSameProgramAreRefused(t *testing.T) {
	err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("scan", "ra8ci:tests-readme"),
		repeatedStep("scan-again", "ra8ci:tests-readme"),
	))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
	if !strings.Contains(err.Error(), "ra8ci:tests-readme") {
		t.Fatalf("refusal does not name the command: %v", err)
	}
}

func TestOneReadOnlyStepIsAdmitted(t *testing.T) {
	if err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("only", "bash", "scripts/ci.sh", "--fast"),
	)); err != nil {
		t.Fatalf("a single step repeats nothing: %v", err)
	}
}

func TestNoStepsIsAdmittedHere(t *testing.T) {
	if err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask()); err != nil {
		t.Fatalf("an empty step list is ValidateTask's refusal, not this one: %v", err)
	}
}

func TestRepeatArgumentsAreComparedElementwiseNotJoined(t *testing.T) {
	if err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("one", "bash", "scripts/ci.sh", "--gate format"),
		repeatedStep("two", "bash", "scripts/ci.sh", "--gate", "format"),
	)); err != nil {
		t.Fatalf("one argument is not two: %v", err)
	}
}

func TestADifferentProgramWithTheSameArgumentsIsAdmitted(t *testing.T) {
	if err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("one", "bash", "--all"),
		repeatedStep("two", "sh", "--all"),
	)); err != nil {
		t.Fatalf("the program is part of the command: %v", err)
	}
}

func TestAThirdRepeatIsStillRefused(t *testing.T) {
	err := checkNoStepRepeatsAnotherStepsCommand(readOnlyRepeatTask(
		repeatedStep("one", "bash", "scripts/ci.sh", "--fast"),
		repeatedStep("two", "bash", "scripts/ci.sh", "--fast"),
		repeatedStep("three", "bash", "scripts/ci.sh", "--fast"),
	))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
}

func TestTheShippedCatalogRunsNoCommandTwiceInOneTask(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	for _, name := range loaded.Names() {
		task, found := loaded.Task(name)
		if !found {
			t.Fatalf("catalog names %q and does not hold it", name)
		}
		if err := checkNoStepRepeatsAnotherStepsCommand(task); err != nil {
			t.Fatalf("shipped task %q: %v", name, err)
		}
	}
}

func TestAReviewedReadOnlyTaskRepeatingACommandIsRefusedByTheAdmissionRule(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	var subject Task
	for _, name := range loaded.Names() {
		task, _ := loaded.Task(name)
		if task.Scope == "safe-local-read-only" && len(task.Steps) > 0 &&
			len(task.ArgsSchema.Positional)+len(task.ArgsSchema.Flags) == 0 {
			subject = task
			break
		}
	}
	if subject.Name == "" {
		t.Skip("no read-only task with steps and no declared arguments in the shipped catalog")
	}
	if err := ValidateReviewedTask(subject); err != nil {
		t.Fatalf("shipped task %q does not pass admission unmodified: %v", subject.Name, err)
	}
	repeat := subject.Steps[0]
	repeat.Name = repeat.Name + "-again"
	subject.Steps = append(append([]Step(nil), subject.Steps...), repeat)
	if err := ValidateReviewedTask(subject); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want the admission rule to refuse the repeat, got %v", err)
	}
}
