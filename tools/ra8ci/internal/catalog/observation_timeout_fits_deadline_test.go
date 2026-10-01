package catalog

import (
	"errors"
	"strings"
	"testing"
)

// timeoutHILTask is a reviewed HIL task that declares an observation timeout,
// the only shape this rule has anything to say about. The shared fixture's
// observation step names a program the dispatch seam refuses, which is fine
// for ValidateTask and not for the admission path this rule sits on, so it
// names a reviewed tool instead.
func timeoutHILTask(t *testing.T) Task {
	t.Helper()
	task := handoffHILTask(t)
	task.Steps[len(task.Steps)-1].Program = "ra8ci:ascii"
	task.Steps[len(task.Steps)-1].Args = []string{"--all"} // ascii cannot derive its own scope; see a_scope_selector_the_tool_requires.go
	task.DeadlineSeconds = 300
	task.HIL.TimeoutDeclared = true
	task.HIL.TimeoutSeconds = 60
	return task
}

func TestADeclaredTimeoutLongerThanTheDeadlineIsRefused(t *testing.T) {
	task := timeoutHILTask(t)
	task.DeadlineSeconds = 60
	task.HIL.TimeoutSeconds = 61
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("an observation timeout longer than the deadline was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "61") || !strings.Contains(err.Error(), "60") {
		t.Fatalf("refusal does not name both numbers: %v", err)
	}
}

// The boundary is the deadline itself: an observation that exactly fills the
// attempt is declarable, it simply leaves no room for anything after it.
func TestADeclaredTimeoutExactlyAtTheDeadlineIsAccepted(t *testing.T) {
	task := timeoutHILTask(t)
	task.DeadlineSeconds = 60
	task.HIL.TimeoutSeconds = 60
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("an observation timeout exactly at the deadline was rejected: %v", err)
	}
}

func TestTheTimeoutRuleIsAppliedAcrossDeadlines(t *testing.T) {
	for _, row := range []struct {
		deadline, timeout int
		accepted          bool
	}{
		{deadline: 1, timeout: 1, accepted: true},
		{deadline: 1, timeout: 2, accepted: false},
		{deadline: 30, timeout: 29, accepted: true},
		{deadline: 300, timeout: 300, accepted: true},
		{deadline: 300, timeout: 301, accepted: false},
		{deadline: 3600, timeout: 3600, accepted: true},
		{deadline: 86400, timeout: 3600, accepted: true},
	} {
		task := timeoutHILTask(t)
		task.DeadlineSeconds = row.deadline
		task.HIL.TimeoutSeconds = row.timeout
		err := ValidateReviewedTask(task)
		if row.accepted && err != nil {
			t.Fatalf("deadline %ds timeout %ds rejected: %v", row.deadline, row.timeout, err)
		}
		if !row.accepted && !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("deadline %ds timeout %ds accepted: %v", row.deadline, row.timeout, err)
		}
	}
}

// Undeclared is a real answer, not a gap to fill in with a default: the 30s
// hilpolicy fallback is the policy layer's choice for a task that states
// nothing, and a task shorter than it is not refused for a number its own
// definition never claimed.
func TestAnUndeclaredTimeoutIsNotJudgedAgainstTheDeadline(t *testing.T) {
	task := timeoutHILTask(t)
	task.HIL.TimeoutDeclared = false
	task.HIL.TimeoutSeconds = 0
	task.DeadlineSeconds = 1
	if err := checkObservationTimeoutFitsTheDeadline(task); err != nil {
		t.Fatalf("an undeclared timeout was judged against the deadline: %v", err)
	}
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a task declaring no timeout was refused for the default: %v", err)
	}
}

// The safety maximum binds by being the smaller of several caps, so one above
// the deadline is inert rather than contradictory and is deliberately left
// alone.
func TestASafetyMaximumAboveTheDeadlineIsStillAccepted(t *testing.T) {
	task := timeoutHILTask(t)
	task.DeadlineSeconds = 120
	task.HIL.TimeoutSeconds = 60
	task.HIL.SafetyMaximumSeconds = 3600
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a safety maximum above the deadline was rejected: %v", err)
	}
}

// A non-HIL task carries no HIL block at all, so there is nothing to hold
// under the deadline and no deadline short enough to make one.
func TestANonHILTaskIsNotJudgedByThisRule(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	task, found := loaded.Task("format")
	if !found {
		t.Fatal("format fixture not found")
	}
	task.DeadlineSeconds = 1
	if task.HIL != nil {
		t.Fatal("fixture carries a HIL block")
	}
	if err := checkObservationTimeoutFitsTheDeadline(task); err != nil {
		t.Fatalf("a non-HIL task was judged by the HIL timeout rule: %v", err)
	}
}

// The window the refused definition would produce is at least its declared
// timeout, which is what makes the contradiction fatal rather than cosmetic.
// This transcribes the two rules that matter rather than calling them: the
// fallback floor hilpolicy.Choose applies, and the deadline ceiling both
// validHILTimingAssignment and validateHILTimingEvidence apply.
func TestARefusedDefinitionWouldProduceAWindowBothDoorsReject(t *testing.T) {
	for _, row := range []struct{ deadline, timeout int }{
		{deadline: 1, timeout: 2},
		{deadline: 60, timeout: 61},
		{deadline: 60, timeout: 3600},
		{deadline: 299, timeout: 300},
	} {
		// hilpolicy.Choose never returns below the declared fallback.
		window := row.timeout
		// Both assignment doors refuse a window wider than the deadline.
		if window <= row.deadline {
			t.Fatalf("transcribed window %ds does not exceed deadline %ds", window, row.deadline)
		}
		task := timeoutHILTask(t)
		task.DeadlineSeconds = row.deadline
		task.HIL.TimeoutSeconds = row.timeout
		if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("deadline %ds timeout %ds admitted though every assignment refuses it: %v",
				row.deadline, row.timeout, err)
		}
	}
}

// Changing only the timeout decides the outcome: the rest of the fixture is
// admissible on its own.
func TestTheTimeoutIsTheOnlyFieldThisRuleTurnsOn(t *testing.T) {
	task := timeoutHILTask(t)
	task.DeadlineSeconds = 100
	task.HIL.TimeoutSeconds = 100
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("fixture is not admissible: %v", err)
	}
	task.HIL.TimeoutSeconds = 101
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("one extra second did not decide the outcome: %v", err)
	}
}

// The rule sits on admission, not on the metadata check store re-applies to
// definitions already running under a reviewed digest.
func TestHeldDefinitionsAreNotRetroactivelyRefused(t *testing.T) {
	task := timeoutHILTask(t)
	task.DeadlineSeconds = 60
	task.HIL.TimeoutSeconds = 600
	if err := ValidateHILTaskMetadata(*task.HIL); err != nil {
		t.Fatalf("the held-definition check refused a contradiction it cannot see: %v", err)
	}
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("admission accepted it: %v", err)
	}
}
