package catalog

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// observedHILTask is a reviewed HIL task with declared handoff bounds AND a
// declared observation timeout, the only shape this rule has anything to say
// about. The shared fixture's observation step names a program the dispatch
// seam refuses, so it names a reviewed tool instead.
func observedHILTask(t *testing.T) Task {
	t.Helper()
	task := handoffHILTask(t)
	task.Steps[len(task.Steps)-1].Program = "ra8ci:ascii"
	task.Steps[len(task.Steps)-1].Args = []string{"--all"} // ascii cannot derive its own scope; see a_scope_selector_the_tool_requires.go
	task.DeadlineSeconds = 600
	task.HIL.FlashRestoreSeconds = 10
	task.HIL.TimeoutDeclared = true
	task.HIL.TimeoutSeconds = 30
	task.HIL.HandoffSafeStepSeconds = 30
	task.HIL.HandoffRestoreProbeSeconds = 30
	return task
}

func TestASafeStepUnderTheObservationTimeoutIsRefused(t *testing.T) {
	task := observedHILTask(t)
	task.HIL.TimeoutSeconds = 300
	task.HIL.HandoffSafeStepSeconds = 20
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a safe step under the observation timeout was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "20") || !strings.Contains(err.Error(), "300") {
		t.Fatalf("refusal does not name both numbers: %v", err)
	}
	if !strings.Contains(err.Error(), task.Name) {
		t.Fatalf("refusal does not name the task: %v", err)
	}
}

// The boundary is equality: a safe step exactly as long as the observation
// says it may run is the tightest honest answer, not a contradiction.
func TestASafeStepExactlyAtTheObservationTimeoutIsAccepted(t *testing.T) {
	task := observedHILTask(t)
	task.HIL.TimeoutSeconds = 45
	task.HIL.HandoffSafeStepSeconds = 45
	task.HIL.SafetyMaximumSeconds = 45
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a safe step exactly at the observation timeout was rejected: %v", err)
	}
}

func TestASafeStepAboveTheObservationTimeoutIsAccepted(t *testing.T) {
	task := observedHILTask(t)
	task.HIL.TimeoutSeconds = 30
	task.HIL.HandoffSafeStepSeconds = 120
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a safe step above the observation timeout was rejected: %v", err)
	}
}

func TestTheRuleIsAppliedAcrossObservationBudgets(t *testing.T) {
	for _, row := range []struct {
		timeout, safeStep int
		accepted          bool
	}{
		{timeout: 1, safeStep: 1, accepted: true},
		{timeout: 2, safeStep: 1, accepted: false},
		{timeout: 30, safeStep: 29, accepted: false},
		{timeout: 30, safeStep: 30, accepted: true},
		{timeout: 600, safeStep: 60, accepted: false},
		{timeout: 60, safeStep: 600, accepted: true},
		{timeout: 3600, safeStep: 3600, accepted: true},
	} {
		task := observedHILTask(t)
		task.DeadlineSeconds = 86400
		task.HIL.TimeoutSeconds = row.timeout
		task.HIL.HandoffSafeStepSeconds = row.safeStep
		task.HIL.SafetyMaximumSeconds = 0
		err := ValidateReviewedTask(task)
		if row.accepted && err != nil {
			t.Fatalf("timeout %ds with a %ds safe step was rejected: %v", row.timeout, row.safeStep, err)
		}
		if !row.accepted && !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("timeout %ds with a %ds safe step was accepted: %v", row.timeout, row.safeStep, err)
		}
	}
}

// An undeclared timeout is a real answer, not 30s with the paperwork missing:
// the policy layer's default is not a number this definition claims, the
// same line checkObservationTimeoutFitsTheDeadline draws.
func TestAnUndeclaredTimeoutIsNotHeldToThePolicyDefault(t *testing.T) {
	task := observedHILTask(t)
	task.HIL.TimeoutDeclared = false
	task.HIL.TimeoutSeconds = 0
	task.HIL.HandoffSafeStepSeconds = 5
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("an undeclared timeout was held to a default: %v", err)
	}
}

// Undeclared handoff bounds stay a real answer: there is no floor to
// understate when the task quotes none.
func TestUndeclaredHandoffBoundsAreNotJudgedAgainstTheTimeout(t *testing.T) {
	task := observedHILTask(t)
	task.HIL.TimeoutSeconds = 300
	task.HIL.HandoffSafeStepSeconds = 0
	task.HIL.HandoffRestoreProbeSeconds = 0
	task.HIL.SafetyMaximumSeconds = 300
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("undeclared handoff bounds were judged against the timeout: %v", err)
	}
	if task.HIL.HandoffBoundsDeclared() || task.HIL.HandoffSafetyBound() != 0 {
		t.Fatalf("undeclared bounds reported as declared: %+v", task.HIL)
	}
}

func TestANonHILTaskIsUntouchedByTheObservationRule(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	task, found := loaded.Task("format")
	if !found {
		t.Fatal("format fixture not found")
	}
	if err := checkSafeStepCoversTheObservation(task); err != nil {
		t.Fatalf("a non-HIL task was judged: %v", err)
	}
}

// The rule is a cross-field admission rule, so it must not retire a
// definition a runtime already holds: ValidateTask keeps admitting the shape
// ValidateReviewedTask now refuses, the same line the three sibling rules
// take.
func TestTheObservationRuleIsAdmissionOnly(t *testing.T) {
	task := observedHILTask(t)
	task.HIL.TimeoutSeconds = 300
	task.HIL.HandoffSafeStepSeconds = 20
	task.HIL.SafetyMaximumSeconds = 300
	if err := ValidateTask(task); err != nil {
		t.Fatalf("a held definition was retired by an admission rule: %v", err)
	}
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("admission accepted what the rule refuses: %v", err)
	}
}

// The quoted floor is what the rule protects: with the refusal in place, a
// declared safety bound can no longer sit under the observation the same
// definition allows.
func TestTheAdmittedSafetyBoundCoversTheObservation(t *testing.T) {
	task := observedHILTask(t)
	task.HIL.TimeoutSeconds = 45
	task.HIL.HandoffSafeStepSeconds = 45
	task.HIL.HandoffRestoreProbeSeconds = 20
	task.HIL.SafetyMaximumSeconds = 45
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatal(err)
	}
	if bound := task.HIL.HandoffSafetyBound(); bound < time.Duration(task.HIL.TimeoutSeconds)*time.Second {
		t.Fatalf("admitted safety bound %s sits under the %ds observation", bound, task.HIL.TimeoutSeconds)
	}
}

// A safe step is judged against the observation and the deadline
// independently: neither refusal stands in for the other.
func TestTheObservationRuleDoesNotReplaceTheDeadlineRule(t *testing.T) {
	task := observedHILTask(t)
	task.DeadlineSeconds = 20
	task.HIL.TimeoutDeclared = false
	task.HIL.TimeoutSeconds = 0
	task.HIL.HandoffSafeStepSeconds = 100
	task.HIL.SafetyMaximumSeconds = 0
	if err := checkSafeStepCoversTheObservation(task); err != nil {
		t.Fatalf("the observation rule judged a task with no declared timeout: %v", err)
	}
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a safe step outlasting the deadline was accepted: %v", err)
	}
}
