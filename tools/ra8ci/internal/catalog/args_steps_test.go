// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// argumentStepsTask is a reviewed definition the admission rules admit: one
// dispatched script, no declared arguments. Each test below declares the
// arguments and the steps it is actually about.
func argumentStepsTask() Task {
	return Task{
		Name: "rewrite", Version: 1, Tier: "optional", Scope: "safe-local-write-working-tree",
		OS: []string{"linux"}, DeadlineSeconds: 300, BoardPolicy: "none",
		Steps: []Step{{Name: "rewrite-path", Program: DispatchShell,
			Args: []string{"scripts/checks/rewrite" + ScriptPathSuffix}}},
		Retry: RetryPolicy{MaxAttempts: 1},
	}
}

func secondStep() Step {
	return Step{Name: "selftest", Program: DispatchShell,
		Args: []string{"scripts/checks/selftest" + ScriptPathSuffix}}
}

func TestATaskWithOneStepMayDeclareArguments(t *testing.T) {
	task := argumentStepsTask()
	task.ArgsSchema = ArgsSchema{Positional: []string{"path"}, Flags: []string{"mode"}}
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a single-step task taking arguments must be admitted: %v", err)
	}
}

func TestATaskWithManyStepsAndNoArgumentsIsStillAdmitted(t *testing.T) {
	// Eighteen tasks in the v1 catalog run several steps. None declares an
	// argument, and this rule must leave every one of them alone.
	task := argumentStepsTask()
	task.Steps = append(task.Steps, secondStep())
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a multi-step task declaring no arguments must be admitted: %v", err)
	}
}

func TestATaskWithManyStepsMayNotDeclareArguments(t *testing.T) {
	task := argumentStepsTask()
	task.ArgsSchema = ArgsSchema{Positional: []string{"path"}}
	task.Steps = append(task.Steps, secondStep())
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	for _, want := range []string{"rewrite", "1 argument(s)", "2 step(s)"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal must name %q, got %v", want, err)
		}
	}
}

// A flags-only schema is the quiet case: nothing is required, so such a task
// would run to a clean exit with the flag appended to a step that never
// declared it.
func TestAFlagsOnlySchemaIsRefusedOnAManyStepTaskToo(t *testing.T) {
	task := argumentStepsTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task.Steps = append(task.Steps, secondStep())
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
}

// The rule counts every declared name, so a schema at the far end of both
// bounds is refused the same way one positional is.
func TestTheRefusalCountsPositionalsAndFlagsTogether(t *testing.T) {
	task := argumentStepsTask()
	task.ArgsSchema = ArgsSchema{Positional: []string{"path", "target"}, Flags: []string{"mode"}}
	task.Steps = append(task.Steps, secondStep())
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	if !strings.Contains(err.Error(), "3 argument(s)") {
		t.Fatalf("refusal must count both halves of the schema, got %v", err)
	}
}

// ValidateTask is the runtime re-check, and it must not gain this rule: a
// definition admitted under an older catalog keeps running, the same line the
// dispatch seam already draws.
func TestTheRuntimeRecheckDoesNotApplyTheRule(t *testing.T) {
	task := argumentStepsTask()
	task.ArgsSchema = ArgsSchema{Positional: []string{"path"}}
	task.Steps = append(task.Steps, secondStep())
	if err := ValidateTask(task); err != nil {
		t.Fatalf("the behavior rules must still admit the task: %v", err)
	}
}

// A behavior refusal is reported before this one, so a definition that is not
// a task at all is named as such rather than as an argument problem.
func TestBehaviorRefusalsStillComeFirst(t *testing.T) {
	task := argumentStepsTask()
	task.ArgsSchema = ArgsSchema{Positional: []string{"path"}}
	task.Steps = append(task.Steps, secondStep())
	task.Tier = "whenever"
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) || !strings.Contains(err.Error(), "invalid tier") {
		t.Fatalf("expected the behavior refusal, got %v", err)
	}
}

// A task with no steps is ValidateTask's refusal, not this one, whatever its
// schema declares.
func TestAStepLessTaskIsRefusedAsHavingNoSteps(t *testing.T) {
	task := argumentStepsTask()
	task.ArgsSchema = ArgsSchema{Positional: []string{"path"}}
	task.Steps = nil
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) || !strings.Contains(err.Error(), "no steps") {
		t.Fatalf("expected the no-steps refusal, got %v", err)
	}
}

// The embedded catalog must pass the new rule as it stands today: ascii-rewrite
// is the only task declaring a schema and it runs one step.
func TestEveryEmbeddedTaskReachesOneStepWithItsArguments(t *testing.T) {
	c, err := Load()
	if err != nil {
		t.Fatalf("load embedded catalog: %v", err)
	}
	for _, name := range c.Names() {
		task, found := c.Task(name)
		if !found {
			t.Fatalf("catalog names %q and does not carry it", name)
		}
		if err := checkArgumentsReachOneStep(task); err != nil {
			t.Fatalf("embedded task %q: %v", name, err)
		}
	}
}
