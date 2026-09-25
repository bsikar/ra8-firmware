package boardclient

import (
	"context"
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func heartbeatReply(state board.Snapshot, silence, interval time.Duration, overdue bool) map[string]any {
	seen := time.Now().UTC().Add(-silence)
	due := seen.Add(board.HeartbeatGraceBeats * interval)
	expires := state.Lease.ExpiresAt
	return map[string]any{
		"snapshot": state,
		"events":   []board.Event{},
		"liveness": map[string]any{
			"held": true, "lease_id": state.Lease.ID, "holder": state.Lease.Holder,
			"last_seen_at": seen, "beat": true,
			"silence_seconds": silence.Seconds(), "interval_seconds": interval.Seconds(),
			"next_beat_by": due, "overdue": overdue, "expires_at": expires,
			"explain": "holder " + state.Lease.Holder + " last reported " + silence.String() + " ago",
		},
	}
}

func TestHeartbeatReportsLivenessAgainstTheVersionItJustRead(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	var posted struct {
		ExpectedVersion uint64 `json:"expected_version"`
		Generation      uint64 `json:"generation"`
	}
	reads, beats := 0, 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			reads++
			jsonResponse(w, http.StatusOK, state)
			return
		}
		beats++
		if r.URL.Path != "/v1/boards/ek-ra8d2/leases/"+token.LeaseID+"/heartbeat" {
			t.Errorf("beat went to the wrong lease: %s", r.URL.Path)
		}
		if err := json.NewDecoder(r.Body).Decode(&posted); err != nil {
			t.Errorf("beat body did not decode: %v", err)
		}
		jsonResponse(w, http.StatusOK, heartbeatReply(state, 20*time.Second, time.Minute, false))
	})
	defer closeServer()

	snapshot, liveness, err := c.Heartbeat(context.Background(), token)
	if err != nil || reads != 1 || beats != 1 {
		t.Fatalf("the holder could not report itself alive: reads=%d beats=%d err=%v", reads, beats, err)
	}
	if posted.ExpectedVersion != state.Version || posted.Generation != token.Generation {
		t.Fatalf("the beat was not fenced to the board it read: %+v", posted)
	}
	if snapshot.Lease == nil || snapshot.Lease.ID != token.LeaseID {
		t.Fatalf("the beat returned another board: %+v", snapshot)
	}
	if !liveness.Held || liveness.Holder != state.Lease.Holder || !liveness.Beat || liveness.Overdue {
		t.Fatalf("liveness did not decode: %+v", liveness)
	}
	// Seconds on the wire, durations in the client.
	if liveness.Silence != 20*time.Second || liveness.Interval != time.Minute {
		t.Fatalf("durations were not decoded from seconds: %+v", liveness)
	}
	if !liveness.NextBeatBy.Equal(liveness.LastSeenAt.Add(board.HeartbeatGraceBeats * time.Minute)) {
		t.Fatalf("the next beat was not due a grace after the last: %+v", liveness)
	}
	if !liveness.ExpiresAt.Equal(state.Lease.ExpiresAt) {
		t.Fatalf("the beat moved the expiry: %+v", liveness)
	}
}

func TestHeartbeatFromASupersededHolderNeverReachesTheServer(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	token.Generation++
	beats := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			beats++
		}
		jsonResponse(w, http.StatusOK, state)
	})
	defer closeServer()
	if _, _, err := c.Heartbeat(context.Background(), token); err != ErrStaleLease || beats != 0 {
		t.Fatalf("a superseded holder beat at the board anyway: beats=%d err=%v", beats, err)
	}
}

func TestHeartbeatRereadsTheVersionAfterAConcurrentTransition(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	beats := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		beats++
		if beats == 1 {
			jsonResponse(w, http.StatusConflict, map[string]any{
				"code": "conflict", "detail": "board version 41, expected 40", "retryable": true})
			return
		}
		jsonResponse(w, http.StatusOK, heartbeatReply(state, time.Second, time.Minute, false))
	})
	defer closeServer()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if _, liveness, err := c.Heartbeat(ctx, token); err != nil || !liveness.Held || beats != 2 {
		t.Fatalf("a version conflict was not retried: beats=%d err=%v", beats, err)
	}
}

func TestHeartbeatRefusesAReportAboutAnotherLease(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		reply := heartbeatReply(state, time.Second, time.Minute, false)
		reply["liveness"].(map[string]any)["lease_id"] = "01996f90-3415-7cfe-8ff1-600058131b00"
		jsonResponse(w, http.StatusOK, reply)
	})
	defer closeServer()
	if _, _, err := c.Heartbeat(context.Background(), token); err == nil {
		t.Fatal("a liveness report about another lease was accepted")
	}
}

func TestHeartbeatReportsOverdueWithoutSurrenderingTheLease(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		jsonResponse(w, http.StatusOK, heartbeatReply(state, 25*time.Minute, time.Minute, true))
	})
	defer closeServer()
	snapshot, liveness, err := c.Heartbeat(context.Background(), token)
	if err != nil || !liveness.Overdue || liveness.Silence != 25*time.Minute {
		t.Fatalf("overdue liveness did not come back: %+v err=%v", liveness, err)
	}
	// Overdue is evidence, not a verdict: the lease is still the caller's.
	if snapshot.Lease == nil || snapshot.Lease.ID != token.LeaseID || !liveness.ExpiresAt.After(time.Now().UTC()) {
		t.Fatalf("an overdue report was treated as the end of the lease: %+v", liveness)
	}
}
