package catalog

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// restoreHILTask is a reviewed HIL task with declared handoff bounds on the
// admission path, the only shape this rule has anything to say about. The
// shared fixture's observation step names a program the dispatch seam
// refuses, so it names a reviewed tool instead.
func restoreHILTask(t *testing.T) Task {
	t.Helper()
	task := handoffHILTask(t)
	task.Steps[len(task.Steps)-1].Program = "ra8ci:ascii"
	task.Steps[len(task.Steps)-1].Args = []string{"--all"} // ascii cannot derive its own scope; see a_scope_selector_the_tool_requires.go
	task.DeadlineSeconds = 600
	task.HIL.FlashRestoreSeconds = 10
	task.HIL.HandoffSafeStepSeconds = 12
	task.HIL.HandoffRestoreProbeSeconds = 30
	return task
}

func TestARestoreProbeShorterThanTheFlashRestoreIsRefused(t *testing.T) {
	task := restoreHILTask(t)
	task.HIL.FlashRestoreSeconds = 60
	task.HIL.HandoffRestoreProbeSeconds = 8
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a restore probe under the flash restore was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "8") || !strings.Contains(err.Error(), "60") {
		t.Fatalf("refusal does not name both numbers: %v", err)
	}
	if !strings.Contains(err.Error(), task.Name) {
		t.Fatalf("refusal does not name the task: %v", err)
	}
}

// The boundary is equality: a restore-and-probe exactly as long as the flash
// restore claims the probe is free, which is thin but not a contradiction.
func TestARestoreProbeExactlyAtTheFlashRestoreIsAccepted(t *testing.T) {
	task := restoreHILTask(t)
	task.HIL.FlashRestoreSeconds = 45
	task.HIL.HandoffRestoreProbeSeconds = 45
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a restore probe exactly at the flash restore was rejected: %v", err)
	}
}

func TestTheRuleIsAppliedAcrossRestoreBudgets(t *testing.T) {
	for _, row := range []struct {
		flash, probe int
		accepted     bool
	}{
		{flash: 1, probe: 1, accepted: true},
		{flash: 2, probe: 1, accepted: false},
		{flash: 10, probe: 9, accepted: false},
		{flash: 10, probe: 10, accepted: true},
		{flash: 10, probe: 11, accepted: true},
		{flash: 3600, probe: 3599, accepted: false},
		{flash: 3600, probe: maxHandoffBoundSeconds, accepted: true},
	} {
		task := restoreHILTask(t)
		task.DeadlineSeconds = 86400
		task.HIL.FlashRestoreSeconds = row.flash
		task.HIL.HandoffRestoreProbeSeconds = row.probe
		err := ValidateReviewedTask(task)
		if row.accepted && err != nil {
			t.Fatalf("flash %ds probe %ds rejected: %v", row.flash, row.probe, err)
		}
		if !row.accepted && !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("flash %ds probe %ds accepted: %v", row.flash, row.probe, err)
		}
	}
}

// Undeclared handoff bounds stay a real answer. A task that declares none has
// an unknown ETA and nothing to contradict, however long its flash restore is.
func TestUndeclaredBoundsAreLeftAloneWhateverTheFlashRestore(t *testing.T) {
	task := restoreHILTask(t)
	task.HIL.FlashRestoreSeconds = 3600
	task.HIL.HandoffSafeStepSeconds = 0
	task.HIL.HandoffRestoreProbeSeconds = 0
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a task declaring no handoff bounds was rejected: %v", err)
	}
	if task.HIL.HandoffBoundsDeclared() || task.HIL.HandoffSafetyBound() != 0 {
		t.Fatalf("undeclared bounds reported as declared: %+v", task.HIL)
	}
}

// The safe step is the work the restore interrupts, not work it contains, so
// a safe step under the flash restore is ordinary and this rule says nothing
// about it.
func TestASafeStepUnderTheFlashRestoreIsStillAccepted(t *testing.T) {
	task := restoreHILTask(t)
	task.HIL.FlashRestoreSeconds = 30
	task.HIL.HandoffSafeStepSeconds = 2
	task.HIL.HandoffRestoreProbeSeconds = 30
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a safe step under the flash restore was rejected: %v", err)
	}
}

// A non-HIL task has neither number and is never read by this rule.
func TestANonHILTaskIsNotJudgedByTheRestoreRule(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range loaded.Names() {
		task, _ := loaded.Task(name)
		if task.HIL != nil {
			continue
		}
		if err := checkRestoreProbeCoversTheFlashRestore(task); err != nil {
			t.Fatalf("non-HIL task %q refused by the restore rule: %v", name, err)
		}
	}
}

// The refused shape is exactly the one whose quoted floor lands under the
// restore: the safety bound a waiter is served is the sum of both bounds, and
// with the probe under the flash restore that sum can still sit below the
// restore this task always pays.
func TestTheRefusedShapeIsTheOneThatQuotesTooSoon(t *testing.T) {
	task := restoreHILTask(t)
	task.HIL.FlashRestoreSeconds = 120
	task.HIL.HandoffSafeStepSeconds = 5
	task.HIL.HandoffRestoreProbeSeconds = 10
	if bound := task.HIL.HandoffSafetyBound(); bound >= time.Duration(task.HIL.FlashRestoreSeconds)*time.Second {
		t.Fatalf("fixture does not quote under its own restore: %s", bound)
	}
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the quoting-too-soon shape was accepted: %v", err)
	}
}

// The runtime re-check of a held definition does not apply this rule: a
// definition admitted under an older rule keeps running.
func TestTheHeldDefinitionRecheckDoesNotApplyTheRule(t *testing.T) {
	task := restoreHILTask(t)
	task.HIL.FlashRestoreSeconds = 90
	task.HIL.HandoffRestoreProbeSeconds = 5
	if err := ValidateHILTaskMetadata(*task.HIL); err != nil {
		t.Fatalf("the held-definition re-check refused an already-admitted task: %v", err)
	}
	if err := ValidateTask(task); err != nil {
		t.Fatalf("ValidateTask refused an already-admitted task: %v", err)
	}
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("admission accepted the same shape: %v", err)
	}
}

// Every task compiled into this binary already satisfies the rule.
func TestEveryEmbeddedTaskCoversItsFlashRestore(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range loaded.Names() {
		task, _ := loaded.Task(name)
		if err := checkRestoreProbeCoversTheFlashRestore(task); err != nil {
			t.Fatalf("embedded task %q: %v", name, err)
		}
	}
}
