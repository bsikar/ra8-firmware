// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// slowStatusClient answers one nominated status request slowly. Time passing
// inside a call the attempt makes between steps, rather than inside a step, is
// the only way the attempt deadline lapses at a loop boundary.
type slowStatusClient struct {
	*testSegmentControlClient
	asks    int
	slowAsk int
	nap     time.Duration
}

func (c *slowStatusClient) Status(ctx context.Context, boardID string) (board.Snapshot, error) {
	c.asks++
	if c.asks == c.slowAsk {
		time.Sleep(c.nap)
	}
	return c.testSegmentControlClient.Status(ctx, boardID)
}

// An attempt whose deadline passes between steps stops at the top of the next
// step, before the board is touched again. The step that already finished
// keeps the state it earned: it succeeded, and the attempt around it timed
// out. Reporting that step as timed out would blame the board for time spent
// elsewhere.
func TestAnAttemptWhoseDeadlinePassesBetweenStepsStopsAtTheNextStep(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	slow := &slowStatusClient{testSegmentControlClient: client, slowAsk: 2, nap: 250 * time.Millisecond}
	agent.client = slow
	assignment := hilAttemptAssignment(token.BoardID)
	if len(assignment.Task.Steps) < 2 {
		t.Fatalf("fixture no longer has a step after the first: %d", len(assignment.Task.Steps))
	}
	started := time.Now().UTC()
	assignment.Attempt.StartedAt = started
	assignment.Attempt.DeadlineAt = started.Add(200 * time.Millisecond)
	assignment.HILTiming.Decision.ValidityWindow = 50 * time.Millisecond
	assignment.HILTiming.Decision.FlashRestoreBound = 10 * time.Millisecond
	runs := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(_ context.Context, _ string, _ catalog.Task, _ catalog.Step) (int, error) {
			runs++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if runs != 1 {
		t.Fatalf("the attempt started a step after its deadline: runs = %d", runs)
	}
	if len(completion.Steps) != 1 || completion.Steps[0].State != "succeeded" {
		t.Fatalf("the finished step did not keep the state it earned: %+v", completion.Steps)
	}
	if completion.Result != "timed_out" || !completion.HitDeadline {
		t.Fatalf("the attempt did not report the deadline it passed: %+v", completion)
	}
	if slow.asks < 2 {
		t.Fatalf("the attempt never reconciled between steps: asks = %d", slow.asks)
	}
}
