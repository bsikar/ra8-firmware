package boardclient

import (
	"context"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// beating serves one GET of state and one beat whose reply the caller shapes.
func beating(t *testing.T, state board.Snapshot, shape func(reply map[string]any)) *Client {
	t.Helper()
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		reply := heartbeatReply(state, time.Second, time.Minute, false)
		if shape != nil {
			shape(reply)
		}
		jsonResponse(w, http.StatusOK, reply)
	})
	t.Cleanup(closeServer)
	return c
}

func liveness(reply map[string]any) map[string]any { return reply["liveness"].(map[string]any) }

func TestBeatAcceptsTheServersOwnAnswer(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	snapshot, reported, err := beating(t, state, nil).Heartbeat(context.Background(), token)
	if err != nil {
		t.Fatalf("a well formed beat was refused: %v", err)
	}
	if !beatAnswersThisLease(snapshot, reported, token) {
		t.Fatalf("the rule refuses what the server actually sends: %+v %+v", snapshot, reported)
	}
}

func TestBeatRefusesASnapshotRecordingAnotherLease(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	// The report still names this caller's lease, so only the snapshot
	// half carries the disagreement: the half that was unchecked.
	other := state
	lease := *state.Lease
	lease.ID = "01996f90-3415-7cfe-8ff1-600058131b00"
	other.Lease = &lease
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		reply := heartbeatReply(other, time.Second, time.Minute, false)
		liveness(reply)["lease_id"] = token.LeaseID
		jsonResponse(w, http.StatusOK, reply)
	})
	defer closeServer()
	if _, _, err := c.Heartbeat(context.Background(), token); err == nil {
		t.Fatal("a beat answered with another lease's board was accepted")
	}
}

func TestBeatRefusesASnapshotOnAnotherGeneration(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	moved := state
	lease := *state.Lease
	lease.Generation++
	moved.Lease = &lease
	moved.Generation++
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		jsonResponse(w, http.StatusOK, heartbeatReply(moved, time.Second, time.Minute, false))
	})
	defer closeServer()
	if _, _, err := c.Heartbeat(context.Background(), token); err == nil {
		t.Fatal("a beat answered on a superseded generation was accepted")
	}
}

func TestBeatRefusesASnapshotWithNoLeaseAtAll(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	free, err := board.New(state.BoardID)
	if err != nil {
		t.Fatal(err)
	}
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		jsonResponse(w, http.StatusOK, map[string]any{
			"snapshot": free, "events": []board.Event{},
			"liveness": map[string]any{"held": true, "lease_id": token.LeaseID, "beat": true,
				"silence_seconds": 1.0, "interval_seconds": 60.0, "expires_at": state.Lease.ExpiresAt},
		})
	})
	defer closeServer()
	if _, _, err := c.Heartbeat(context.Background(), token); err == nil {
		t.Fatal("a beat that recorded a free board was accepted as this holder's")
	}
}

func TestBeatRefusesAReportThatSaysTheBoardIsNotHeld(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	c := beating(t, state, func(reply map[string]any) { liveness(reply)["held"] = false })
	if _, _, err := c.Heartbeat(context.Background(), token); err == nil {
		t.Fatal("a beat the server recorded came back reporting nobody holds the board")
	}
}

func TestBeatRefusesADeadlineThatIsNotTheLeasesOwn(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	c := beating(t, state, func(reply map[string]any) {
		liveness(reply)["expires_at"] = state.Lease.ExpiresAt.Add(time.Hour)
	})
	if _, _, err := c.Heartbeat(context.Background(), token); err == nil {
		t.Fatal("a report stating an expiry the lease does not have was accepted")
	}
}

func TestBeatRefusesAMissingDeadline(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	c := beating(t, state, func(reply map[string]any) { liveness(reply)["expires_at"] = nil })
	if _, _, err := c.Heartbeat(context.Background(), token); err == nil {
		t.Fatal("a report stating no expiry for a live lease was accepted")
	}
}

func TestBeatRuleHoldsEveryFieldOfTheToken(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	reported := HolderLiveness{Held: true, LeaseID: token.LeaseID, ExpiresAt: state.Lease.ExpiresAt}
	if !beatAnswersThisLease(state, reported, token) {
		t.Fatal("the agreeing case is refused")
	}
	for name, broken := range map[string]LeaseToken{
		"another board":  {BoardID: "other", RequestID: token.RequestID, LeaseID: token.LeaseID, Generation: token.Generation},
		"another waiter": {BoardID: token.BoardID, RequestID: "other", LeaseID: token.LeaseID, Generation: token.Generation},
		"another lease":  {BoardID: token.BoardID, RequestID: token.RequestID, LeaseID: "other", Generation: token.Generation},
		"another gen":    {BoardID: token.BoardID, RequestID: token.RequestID, LeaseID: token.LeaseID, Generation: token.Generation + 1},
		"no generation":  {BoardID: token.BoardID, RequestID: token.RequestID, LeaseID: token.LeaseID},
	} {
		if beatAnswersThisLease(state, reported, broken) {
			t.Fatalf("a beat was accepted for %s", name)
		}
	}
}

// The rule judges the answer, never the clock: a lease already past its expiry
// is still this caller's lease, and nothing here ends one.
func TestBeatRuleDoesNotJudgeTheDeadlineItReports(t *testing.T) {
	state := activeBoard(t)
	lease := *state.Lease
	lease.ExpiresAt = time.Now().UTC().Add(-time.Hour)
	state.Lease = &lease
	token := testToken(state)
	reported := HolderLiveness{Held: true, LeaseID: token.LeaseID, ExpiresAt: lease.ExpiresAt, Overdue: true}
	if !beatAnswersThisLease(state, reported, token) {
		t.Fatal("an overdue holder's own answer was refused")
	}
}
