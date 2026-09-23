package boardagent

import (
	"context"
	"encoding/json"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type testSegmentControlClient struct {
	*testControlClient
	begins       int
	finishes     []string
	lastSegment  store.BoardSegment
	claimCount   int
	claimedLease string
}

func (c *testSegmentControlClient) BeginSegment(_ context.Context, token boardclient.LeaseToken, attemptID, key string, bound, margin time.Duration) (store.BoardSegment, error) {
	c.begins++
	now := time.Now()
	c.lastSegment = store.BoardSegment{ID: "01996f90-3415-7cfe-8ff1-600058131aff",
		BoardID: token.BoardID, LeaseID: token.LeaseID, Generation: token.Generation, AttemptID: attemptID,
		Key: key, StartedAt: now, DeadlineAt: now.Add(bound), RecoveryMarginMS: uint64(margin.Milliseconds())}
	return c.lastSegment, nil
}

func (c *testSegmentControlClient) ClaimNextHILAttempt(_ context.Context, boardID, leaseID, host string, cores int, ramBytes int64, load float64, facts json.RawMessage) (*store.BoardHILAssignment, error) {
	c.claimCount++
	c.claimedLease = leaseID
	return &store.BoardHILAssignment{Attempt: store.Attempt{ID: "01996f90-3415-7cfe-8ff1-600058131aff",
		TaskID: "01996f90-3415-7cfe-8ff1-600058131b11", AttemptNo: 1, State: "running"}}, nil
}

func (c *testSegmentControlClient) FinishSegment(_ context.Context, _ boardclient.LeaseToken, attemptID, id, outcome string) error {
	if id != c.lastSegment.ID {
		return boardclient.ErrStaleLease
	}
	c.finishes = append(c.finishes, outcome)
	return nil
}

func newActiveSegmentAgent(t *testing.T) (*Agent, *testSegmentControlClient, boardclient.LeaseToken) {
	t.Helper()
	now := time.Now().UTC()
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	waiter := board.Waiter{ID: "01996f90-3415-7cfe-8ff1-600058131afd",
		LeaseID: "01996f90-3415-7cfe-8ff1-600058131afe", Holder: "agent",
		Class: board.ClassAI, Reason: "test HIL", Duration: time.Minute}
	state, _, err = board.Apply(state, board.Enqueue{Actor: "agent", Waiter: waiter}, now)
	if err != nil {
		t.Fatal(err)
	}
	state, _, err = board.Apply(state, board.AcknowledgeGrant{Actor: "board-agent",
		LeaseID: waiter.LeaseID, Generation: state.Generation, InstalledGeneration: state.Generation}, now)
	if err != nil {
		t.Fatal(err)
	}
	highWater, _ := newTestHighWater(t)
	if err := highWater.Advance(state.Generation); err != nil {
		t.Fatal(err)
	}
	client := &testSegmentControlClient{testControlClient: &testControlClient{state: state}}
	agent, err := New(state.BoardID, client, highWater, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	token := boardclient.LeaseToken{BoardID: state.BoardID, RequestID: waiter.ID,
		LeaseID: waiter.LeaseID, Generation: state.Generation, ExpiresAt: state.Lease.ExpiresAt,
		Version: state.Version}
	return agent, client, token
}

func TestRunSegmentRejectsInvalidAttemptBeforeContactingBoard(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	started := false
	if _, err := agent.RunSegment(context.Background(), token, "not-a-uuid", "invalid-attempt",
		time.Second, 0, func(context.Context) error { started = true; return nil }); err == nil {
		t.Fatal("invalid HIL attempt identifier was accepted")
	}
	if started || client.begins != 0 || len(client.finishes) != 0 {
		t.Fatalf("invalid attempt reached hardware path: started=%v begins=%d finishes=%v",
			started, client.begins, client.finishes)
	}
}

func TestClaimNextHILAttemptRequiresActiveFencedLease(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment, err := agent.ClaimNextHILAttempt(context.Background(), token, "hil-runner", 8, 8<<30, 0.5,
		json.RawMessage("{\"os\":\"linux\",\"arch\":\"amd64\"}"))
	if err != nil || assignment == nil || assignment.Attempt.State != "running" ||
		client.claimCount != 1 || client.claimedLease != token.LeaseID {
		t.Fatalf("active HIL task was not claimed under its lease: assignment=%+v claims=%d lease=%s err=%v",
			assignment, client.claimCount, client.claimedLease, err)
	}
	client.state.Phase = board.YieldRequested
	if _, err := agent.ClaimNextHILAttempt(context.Background(), token, "hil-runner", 8, 8<<30, 0.5,
		json.RawMessage("{\"os\":\"linux\"}")); err == nil {
		t.Fatal("HIL task was claimed after the board entered cooperative yield")
	}
	if client.claimCount != 1 {
		t.Fatal("yielded board made another HIL claim")
	}
}

func TestRunSegmentLetsStartedWorkFinishThenStopsAtHumanCheckpoint(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	started := false
	segment, err := agent.RunSegment(context.Background(), token, "01996f90-3415-7cfe-8ff1-600058131aff", "uart-observe", 3*time.Second, time.Second,
		func(ctx context.Context) error {
			if _, ok := ctx.Deadline(); !ok {
				t.Fatal("board operation has no context deadline")
			}
			started = true
			waiter := board.Waiter{ID: "01996f90-3415-7cfe-8ff1-600058131b01",
				LeaseID: "01996f90-3415-7cfe-8ff1-600058131b02", Holder: "human",
				Class: board.ClassHuman, Reason: "human needs board", Duration: time.Minute}
			next, _, applyErr := board.Apply(client.state, board.Enqueue{Actor: "human", Waiter: waiter}, time.Now().UTC())
			client.state = next
			return applyErr
		})
	if err != nil || !started || segment.ID != client.lastSegment.ID || len(client.finishes) != 1 || client.finishes[0] != "completed" {
		t.Fatalf("bounded operation did not finish at its checkpoint: segment=%+v started=%v finishes=%v err=%v", segment, started, client.finishes, err)
	}
	if client.state.Phase != board.YieldRequested {
		t.Fatalf("human wait did not request cooperative yield: phase=%s", client.state.Phase)
	}
	_, err = agent.RunSegment(context.Background(), token, "01996f90-3415-7cfe-8ff1-600058131aff", "must-not-start", time.Second, 0,
		func(context.Context) error { t.Fatal("operation started after human wait"); return nil })
	if err == nil || client.begins != 1 || len(client.finishes) != 1 {
		t.Fatalf("a new segment crossed the human checkpoint: begins=%d finishes=%v err=%v", client.begins, client.finishes, err)
	}
}

func TestRunSegmentDeadlineCancelsAndClosesFailedSegment(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	_, err := agent.RunSegment(context.Background(), token, "01996f90-3415-7cfe-8ff1-600058131aff", "bounded-wait", 50*time.Millisecond, time.Second,
		func(ctx context.Context) error {
			<-ctx.Done()
			return ctx.Err()
		})
	if !errors.Is(err, context.DeadlineExceeded) || len(client.finishes) != 1 || client.finishes[0] != "failed" {
		t.Fatalf("deadline was not applied and durably closed: finishes=%v err=%v", client.finishes, err)
	}
}
