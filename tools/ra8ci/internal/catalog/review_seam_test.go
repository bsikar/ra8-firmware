package catalog

import (
	"errors"
	"strings"
	"testing"
)

// reviewSeamTask is admitted by the behavior rules and refused by the reviewed
// dispatch seam: "echo" is neither an ra8ci tool nor the dispatch shell.
func reviewSeamTask() Task {
	return Task{
		Name: "fixture", Version: 1, Tier: "required", Scope: "safe-local-read-only",
		OS: []string{"linux"}, DeadlineSeconds: 60, BoardPolicy: "none",
		Steps: []Step{{Name: "observe", Program: "echo", Args: []string{"hello"}}},
		Retry: RetryPolicy{MaxAttempts: 1},
	}
}

func TestValidateReviewedTaskAppliesTheDispatchSeam(t *testing.T) {
	task := reviewSeamTask()
	if err := ValidateTask(task); err != nil {
		t.Fatalf("behavior rules must still admit the task: %v", err)
	}
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("admission must apply the dispatch seam, got %v", err)
	}
	if !strings.Contains(err.Error(), "fixture") {
		t.Fatalf("refusal must name the task, got %v", err)
	}
}

func TestValidateReviewedTaskReportsBehaviorRefusalsFirst(t *testing.T) {
	// A task that fails both rules must report the behavior refusal, which is
	// the one that says the definition is not a task at all.
	task := reviewSeamTask()
	task.Tier = "whenever"
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	if !strings.Contains(err.Error(), "invalid tier") {
		t.Fatalf("expected the behavior refusal, got %v", err)
	}
}

func TestValidateReviewedTaskAdmitsAReviewedDefinition(t *testing.T) {
	task := reviewSeamTask()
	task.Steps = []Step{{Name: "observe", Program: DispatchShell, Args: []string{"scripts/checks/observe" + ScriptPathSuffix}}}
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a reviewed definition must be admitted: %v", err)
	}
}

func TestEveryEmbeddedTaskPassesAdmissionAgain(t *testing.T) {
	// Parse admits the embedded catalog through ValidateReviewedTask, so every
	// definition it carries must still pass the pair when asked directly.
	c, err := Load()
	if err != nil {
		t.Fatalf("load embedded catalog: %v", err)
	}
	for _, name := range c.Names() {
		task, found := c.Task(name)
		if !found {
			t.Fatalf("catalog names %q and does not carry it", name)
		}
		if err := ValidateReviewedTask(task); err != nil {
			t.Fatalf("embedded task %q must pass admission: %v", name, err)
		}
	}
}
