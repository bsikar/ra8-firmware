// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// filableTask is a reviewed definition the admission rules admit. Each test
// below states the name or the step count it is actually about.
func filableTask() Task {
	return Task{
		Name: "selftest", Version: 1, Tier: "optional", Scope: "safe-local-read-only",
		OS: []string{"linux"}, DeadlineSeconds: 300, BoardPolicy: "none",
		Steps: []Step{{Name: "selftest-run", Program: DispatchShell,
			Args: []string{"scripts/checks/format_tree" + ScriptPathSuffix}}},
		Retry: RetryPolicy{MaxAttempts: 1},
	}
}

// namedStepsFor builds count steps whose names are what these tests are about.
// Each carries its own ordinal as an argument so no two dispatch the same
// command: this fixture task is read-only, where a repeated command is its own
// refusal (a_step_a_read_only_task_runs_once.go), and that refusal is not what
// a test about the filable step COUNT is asking.
func namedStepsFor(count int) []Step {
	steps := make([]Step, 0, count)
	for i := 0; i < count; i++ {
		ordinal := stepOrdinalName(i)
		steps = append(steps, Step{
			Name: "step-" + ordinal, Program: DispatchShell,
			Args: []string{"scripts/checks/format_tree" + ScriptPathSuffix, ordinal},
		})
	}
	return steps
}

func stepOrdinalName(i int) string {
	digits := "0123456789"
	return string([]byte{digits[i/100%10], digits[i/10%10], digits[i%10]})
}

func TestATaskHistoryCanFileIsAdmitted(t *testing.T) {
	if err := ValidateReviewedTask(filableTask()); err != nil {
		t.Fatalf("an ordinary reviewed task must be admitted: %v", err)
	}
}

func TestTheWidestFilableTaskNameIsAdmitted(t *testing.T) {
	// This door's own bound, driven directly: a name of exactly
	// maxFilableNameBytes is one durable history files. ValidateReviewedTask
	// no longer admits it, because a grant can carry only 64 bytes and the
	// narrower bound wins at admission (a_task_name_a_grant_can_carry.go);
	// TestTheGapBetweenTheGrantAndHistoryIsClosed pins that end to end.
	task := filableTask()
	task.Name = strings.Repeat("a", maxFilableNameBytes)
	if err := checkTheTaskIsOneHistoryCanFile(task); err != nil {
		t.Fatalf("a task name of exactly %d bytes must be filable: %v", maxFilableNameBytes, err)
	}
}

func TestATaskNamedWiderThanHistoryFilesIsRefused(t *testing.T) {
	task := filableTask()
	task.Name = strings.Repeat("a", maxFilableNameBytes+1)
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	for _, want := range []string{"129 bytes", "128"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal must name %q, got %v", want, err)
		}
	}
}

func TestTheWidestFilableStepCountIsAdmitted(t *testing.T) {
	task := filableTask()
	task.Steps = namedStepsFor(maxFilableSteps)
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("exactly %d steps must be admitted: %v", maxFilableSteps, err)
	}
}

func TestATaskDeclaringMoreStepsThanHistoryFilesIsRefused(t *testing.T) {
	task := filableTask()
	task.Steps = namedStepsFor(maxFilableSteps + 1)
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	for _, want := range []string{"selftest", "129 step(s)", "128"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal must name %q, got %v", want, err)
		}
	}
}

func TestTheWidestFilableStepNameIsAdmitted(t *testing.T) {
	task := filableTask()
	task.Steps[0].Name = strings.Repeat("a", maxFilableNameBytes)
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a step name of exactly %d bytes must be admitted: %v", maxFilableNameBytes, err)
	}
}

func TestAStepNamedWiderThanHistoryFilesIsRefused(t *testing.T) {
	task := filableTask()
	task.Steps[0].Name = strings.Repeat("a", maxFilableNameBytes+1)
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	for _, want := range []string{"selftest", "step name", "129 bytes"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal must name %q, got %v", want, err)
		}
	}
}

// The runtime re-check deliberately stays out of this rule. A task admitted
// under an older manifest is already held; refusing it here would retroactively
// withdraw work review admitted, which is the same seam ValidateTask states for
// the dispatch rules.
func TestTheRuntimeRecheckDoesNotJudgeWhatHistoryCanFile(t *testing.T) {
	task := filableTask()
	task.Steps[0].Name = strings.Repeat("a", maxFilableNameBytes+1)
	if err := ValidateTask(task); err != nil {
		t.Fatalf("the runtime re-check must leave an already-held task alone: %v", err)
	}
}

// The embedded catalog is the manifest this rule is judged against in
// production: every task in it must pass, or the binary cannot load its own
// definitions.
func TestTheEmbeddedCatalogIsOneHistoryCanFile(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("the embedded catalog must load: %v", err)
	}
	for _, name := range loaded.Names() {
		task, found := loaded.Task(name)
		if !found {
			t.Fatalf("catalog names %q and does not hold it", name)
		}
		if err := checkTheTaskIsOneHistoryCanFile(task); err != nil {
			t.Fatalf("embedded task %q is not one history can file: %v", name, err)
		}
	}
}
