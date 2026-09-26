package boardagent

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

func TestOrdinaryStepBoundsAreAccepted(t *testing.T) {
	for _, row := range []struct {
		deadline time.Duration
		window   time.Duration
		span     time.Duration
	}{
		{20 * time.Second, 12 * time.Second, time.Minute},
		{time.Nanosecond, 0, time.Nanosecond},
		{maxBoardOperation, maxBoardOperation, maxBoardOperation},
		{maxBoardOperation, maxBoardOperation, 24 * time.Hour},
		{24 * time.Hour, 24 * time.Hour, maxBoardOperation},
		{24 * time.Hour, 2 * maxBoardOperation, 30 * time.Minute},
	} {
		if err := checkStepBoundsAreOperable(row.deadline, row.window, row.span); err != nil {
			t.Fatalf("deadline %s, window %s, span %s was refused: %v", row.deadline, row.window, row.span, err)
		}
	}
}

// Both halves are in policy where they are set: catalog admits a task deadline
// up to 86400 seconds, and the pinned window arrives from the server.
func TestABoundAboveTheCeilingIsRefusedAndNamed(t *testing.T) {
	for _, row := range []struct {
		deadline time.Duration
		window   time.Duration
		span     time.Duration
		names    string
	}{
		{2 * maxBoardOperation, time.Second, 2 * maxBoardOperation, "task deadline"},
		{24 * time.Hour, time.Second, 24 * time.Hour, "task deadline"},
		{maxBoardOperation + time.Nanosecond, 0, 24 * time.Hour, "task deadline"},
		{20 * time.Second, 2 * maxBoardOperation, 2 * maxBoardOperation, "validity window"},
		{20 * time.Second, maxBoardOperation + time.Nanosecond, 24 * time.Hour, "validity window"},
	} {
		err := checkStepBoundsAreOperable(row.deadline, row.window, row.span)
		if !errors.Is(err, ErrInvalidAgent) {
			t.Fatalf("deadline %s, window %s, span %s was accepted: %v", row.deadline, row.window, row.span, err)
		}
		if !strings.Contains(err.Error(), row.names) {
			t.Fatalf("refusal does not name which bound was too wide: %v", err)
		}
	}
}

// The clamp is the whole reason this is not simply a bound on the catalog's
// own field: a task deadline wider than the attempt never reaches a segment.
func TestAWideDeadlineUnderAShortAttemptIsOrdinary(t *testing.T) {
	for _, span := range []time.Duration{time.Nanosecond, time.Second, 20 * time.Minute,
		maxBoardOperation - time.Nanosecond, maxBoardOperation} {
		if err := checkStepBoundsAreOperable(24*time.Hour, 24*time.Hour, span); err != nil {
			t.Fatalf("span %s under a day-long deadline was refused: %v", span, err)
		}
	}
	if err := checkStepBoundsAreOperable(24*time.Hour, 0, maxBoardOperation+time.Nanosecond); err == nil {
		t.Fatal("a span one nanosecond past the ceiling still clamped inside it")
	}
}

func TestAnAttemptWithNoSpanOrNoDeadlineIsRefused(t *testing.T) {
	for _, row := range []struct {
		deadline time.Duration
		span     time.Duration
		names    string
	}{
		{20 * time.Second, 0, "deadline does not follow"},
		{20 * time.Second, -time.Second, "deadline does not follow"},
		{0, time.Minute, "no positive deadline"},
		{-time.Second, time.Minute, "no positive deadline"},
	} {
		err := checkStepBoundsAreOperable(row.deadline, time.Second, row.span)
		if !errors.Is(err, ErrInvalidAgent) {
			t.Fatalf("deadline %s with span %s was accepted: %v", row.deadline, row.span, err)
		}
		if !strings.Contains(err.Error(), row.names) {
			t.Fatalf("refusal does not say what was missing: %v", err)
		}
	}
}

// This check exists to ask early exactly what RunSegment asks late, so the two
// are crossed here rather than left to agree by eye.
func TestTheEarlyBoundCheckMatchesTheSegmentDoor(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	for _, bound := range []time.Duration{time.Millisecond, time.Second, 30 * time.Minute,
		maxBoardOperation - time.Second, maxBoardOperation, maxBoardOperation + time.Second, 2 * maxBoardOperation} {
		early := checkStepBoundsAreOperable(bound, 0, 24*time.Hour) != nil
		begins := client.begins
		_, segmentErr := agent.RunSegment(context.Background(), token, "01996f90-3415-7cfe-8ff1-600058131aff",
			"bound-cross", bound, 0, func(context.Context) error { return nil })
		door := errors.Is(segmentErr, ErrInvalidAgent) && client.begins == begins
		if early != door {
			t.Fatalf("bound %s: early check refuses=%v, segment door refuses=%v (err %v)",
				bound, early, door, segmentErr)
		}
	}
}

// The observation step runs after the board has been programmed, so a window
// refused inside the loop is a refusal delivered with a live fixture.
func TestAnUnusableObservationWindowStopsTheAttemptBeforeAnyStepRuns(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	started := assignment.Attempt.StartedAt
	assignment.Attempt.DeadlineAt = started.Add(3 * time.Hour)
	assignment.HILTiming.Decision.ValidityWindow = 2 * maxBoardOperation
	steps := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		0, time.Second, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			steps++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if steps != 0 || client.begins != 0 {
		t.Fatalf("the attempt reached the board before its bounds were judged: steps=%d begins=%d", steps, client.begins)
	}
	if completion.Result != "failed" || completion.EvidenceComplete {
		t.Fatalf("an unusable observation window did not fail the attempt: %+v", completion)
	}
	if !strings.Contains(completion.Reason, "validity window") {
		t.Fatalf("recorded reason does not name the bound that was too wide: %q", completion.Reason)
	}
}

// A task deadline above the ceiling is refused only when the attempt is wide
// enough to actually ask for it, which is the shape that made this worth
// finding early: the same task runs or is refused depending on when it starts.
func TestAWideTaskDeadlineIsJudgedAgainstTheAttemptItRunsUnder(t *testing.T) {
	for _, row := range []struct {
		span    time.Duration
		refused bool
	}{
		{20 * time.Second, false},
		{3 * time.Hour, true},
	} {
		agent, client, token := newActiveSegmentAgent(t)
		assignment := hilAttemptAssignment(token.BoardID)
		assignment.Task.DeadlineSeconds = 7200
		assignment.Attempt.DeadlineAt = assignment.Attempt.StartedAt.Add(row.span)
		if err := catalog.ValidateTask(assignment.Task); err != nil {
			t.Fatalf("a two-hour task deadline is not in policy for the catalog: %v", err)
		}
		steps := 0
		completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
			0, time.Second, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
				steps++
				return 0, nil
			})
		if err != nil {
			t.Fatalf("terminal evidence was not persisted: %v", err)
		}
		named := strings.Contains(completion.Reason, "task deadline bounds a segment")
		if named != row.refused {
			t.Fatalf("span %s: refused=%v, reason %q", row.span, named, completion.Reason)
		}
		if !row.refused && (steps != len(assignment.Task.Steps) || client.begins != len(assignment.Task.Steps)) {
			t.Fatalf("span %s: an ordinary attempt did not run its steps: steps=%d begins=%d reason=%q",
				row.span, steps, client.begins, completion.Reason)
		}
		if row.refused && (steps != 0 || client.begins != 0) {
			t.Fatalf("span %s: the attempt reached the board anyway: steps=%d begins=%d", row.span, steps, client.begins)
		}
	}
}

// Changing nothing but the two bounds decides this, so the rule cannot be
// passing for some other reason.
func TestOnlyTheBoundsDecideThisRule(t *testing.T) {
	span := 24 * time.Hour
	if err := checkStepBoundsAreOperable(maxBoardOperation, maxBoardOperation, span); err != nil {
		t.Fatalf("both bounds at the ceiling were refused: %v", err)
	}
	if err := checkStepBoundsAreOperable(maxBoardOperation+time.Nanosecond, maxBoardOperation, span); err == nil {
		t.Fatal("a task deadline one nanosecond past the ceiling was accepted")
	}
	if err := checkStepBoundsAreOperable(maxBoardOperation, maxBoardOperation+time.Nanosecond, span); err == nil {
		t.Fatal("a validity window one nanosecond past the ceiling was accepted")
	}
}
