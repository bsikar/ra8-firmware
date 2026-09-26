package catalog

import (
	"errors"
	"strings"
	"testing"
)

// deadlineHILTask is a reviewed HIL task with declared handoff bounds, the
// only shape this rule has anything to say about.
func deadlineHILTask(t *testing.T) Task {
	t.Helper()
	task := handoffHILTask(t)
	// The shared fixture's observation step names a program the dispatch
	// seam refuses, which is fine for ValidateTask and not for the
	// admission path this rule sits on, so name a reviewed tool instead.
	task.Steps[len(task.Steps)-1].Program = "ra8ci:ascii"
	task.Steps[len(task.Steps)-1].Args = nil
	task.HIL.HandoffSafeStepSeconds = 12
	task.HIL.HandoffRestoreProbeSeconds = 8
	task.DeadlineSeconds = 120
	return task
}

func TestASafeStepLongerThanTheDeadlineIsRefused(t *testing.T) {
	task := deadlineHILTask(t)
	task.DeadlineSeconds = 60
	task.HIL.HandoffSafeStepSeconds = 61
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a safe step longer than the deadline was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "61") || !strings.Contains(err.Error(), "60") {
		t.Fatalf("refusal does not name both numbers: %v", err)
	}
}

// The boundary is the deadline itself: a step that exactly fills the attempt
// is declarable, it simply leaves no room for anything after it.
func TestASafeStepExactlyAtTheDeadlineIsAccepted(t *testing.T) {
	task := deadlineHILTask(t)
	task.DeadlineSeconds = 60
	task.HIL.HandoffSafeStepSeconds = 60
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a safe step exactly at the deadline was rejected: %v", err)
	}
}

func TestTheRuleIsAppliedAcrossDeadlines(t *testing.T) {
	for _, row := range []struct {
		deadline, safeStep int
		accepted           bool
	}{
		{deadline: 1, safeStep: 1, accepted: true},
		{deadline: 1, safeStep: 2, accepted: false},
		{deadline: 300, safeStep: 299, accepted: true},
		{deadline: 300, safeStep: 300, accepted: true},
		{deadline: 300, safeStep: 301, accepted: false},
		{deadline: 86400, safeStep: maxHandoffBoundSeconds, accepted: true},
	} {
		task := deadlineHILTask(t)
		task.DeadlineSeconds = row.deadline
		task.HIL.HandoffSafeStepSeconds = row.safeStep
		task.HIL.HandoffRestoreProbeSeconds = 1
		err := ValidateReviewedTask(task)
		if row.accepted && err != nil {
			t.Fatalf("deadline %ds safe step %ds rejected: %v", row.deadline, row.safeStep, err)
		}
		if !row.accepted && !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("deadline %ds safe step %ds accepted: %v", row.deadline, row.safeStep, err)
		}
	}
}

// Only the safe step is judged. The restore probe is the work that follows
// the step, and this tree budgets a restore outside the attempt deadline, so
// a safety bound reaching past it is ordinary.
func TestASafetyBoundReachingPastTheDeadlineIsStillAccepted(t *testing.T) {
	task := deadlineHILTask(t)
	task.DeadlineSeconds = 60
	task.HIL.HandoffSafeStepSeconds = 50
	task.HIL.HandoffRestoreProbeSeconds = 40
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a safety bound past the deadline was rejected: %v", err)
	}
}

// Undeclared bounds are a real answer, not a gap to judge: there is no
// declared step to hold under anything.
func TestUndeclaredBoundsAreNotJudgedAgainstTheDeadline(t *testing.T) {
	task := handoffHILTask(t)
	task.DeadlineSeconds = 1
	if task.HIL.HandoffBoundsDeclared() {
		t.Fatal("fixture declares bounds")
	}
	if err := checkSafeStepFitsTheDeadline(task); err != nil {
		t.Fatalf("undeclared bounds judged against the deadline: %v", err)
	}
}

func TestANonHILTaskCarriesNothingToJudge(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range loaded.Names() {
		task, _ := loaded.Task(name)
		if task.HIL != nil {
			continue
		}
		task.DeadlineSeconds = 1
		if err := checkSafeStepFitsTheDeadline(task); err != nil {
			t.Fatalf("non-HIL task %q judged against the deadline: %v", name, err)
		}
	}
}

// The rule reads a field outside the HIL block, so it belongs at admission and
// NOT in the runtime re-check: a definition admitted under an older rule keeps
// running, the same line ValidateTask draws against the dispatch seam.
func TestTheRuntimeRecheckStillAcceptsADefinitionItAlreadyHolds(t *testing.T) {
	task := deadlineHILTask(t)
	task.DeadlineSeconds = 60
	task.HIL.HandoffSafeStepSeconds = 600
	if err := ValidateTask(task); err != nil {
		t.Fatalf("the runtime re-check retroactively refused a held definition: %v", err)
	}
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("admission accepted it: %v", err)
	}
}

// The rule is stated once and reached through admission, not restated inline.
func TestAdmissionAppliesExactlyTheRuleTheCheckStates(t *testing.T) {
	for _, safeStep := range []int{1, 59, 60, 61, 600} {
		task := deadlineHILTask(t)
		task.DeadlineSeconds = 60
		task.HIL.HandoffSafeStepSeconds = safeStep
		direct := checkSafeStepFitsTheDeadline(task)
		admitted := ValidateReviewedTask(task)
		if (direct == nil) != (admitted == nil) {
			t.Fatalf("safe step %ds: check says %v, admission says %v", safeStep, direct, admitted)
		}
	}
}

// Nothing in the reviewed v1 catalog is refused by this rule.
func TestTheEmbeddedCatalogSatisfiesTheRule(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range loaded.Names() {
		task, _ := loaded.Task(name)
		if err := checkSafeStepFitsTheDeadline(task); err != nil {
			t.Fatalf("reviewed task %q refused: %v", name, err)
		}
	}
}
