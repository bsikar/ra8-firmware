package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func heldBoard(beat time.Time) board.Snapshot {
	granted := time.Now().UTC().Add(-30 * time.Minute)
	return board.Snapshot{
		BoardID:        "ek-ra8d2",
		Phase:          board.Active,
		Generation:     7,
		AgentHighWater: 7,
		Version:        41,
		Lease: &board.Lease{
			ID: boardTestLeaseID, WaiterID: boardTestRequestID, Holder: "runner-3",
			Class: board.ClassCI, Reason: "hil run", Generation: 7,
			GrantedAt: granted, ExpiresAt: granted.Add(2 * time.Hour), RequestedDuration: 2 * time.Hour,
			DeadlineVersion: 1, LastHeartbeatAt: beat,
		},
	}
}

func heartbeatPath() string {
	return "/v1/boards/ek-ra8d2/leases/" + boardTestLeaseID + "/heartbeat"
}

func decodeLiveness(t *testing.T, body []byte) holderLivenessView {
	t.Helper()
	var reply struct {
		Liveness holderLivenessView `json:"liveness"`
	}
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("heartbeat reply did not decode: %v", err)
	}
	return reply.Liveness
}

func TestHeartbeatRecordsTheBeatAndReportsWhenTheNextIsDue(t *testing.T) {
	seen := time.Now().UTC().Add(-20 * time.Second)
	f := &fakeBoardStore{board: heldBoard(seen)}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", heartbeatPath(),
		`{"expected_version":41,"generation":7}`))
	if w.Code != http.StatusOK || f.applies != 1 {
		t.Fatalf("the holder could not report itself alive: status=%d applies=%d body=%s", w.Code, f.applies, w.Body)
	}
	command, ok := f.command.(board.HolderHeartbeat)
	if !ok || command.LeaseID != boardTestLeaseID || command.Generation != 7 || f.version != 41 {
		t.Fatalf("the beat was not fenced to the lease it named: %+v version=%d", f.command, f.version)
	}
	liveness := decodeLiveness(t, w.Body.Bytes())
	if !liveness.Held || liveness.Holder != "runner-3" || liveness.LeaseID != boardTestLeaseID {
		t.Fatalf("the report did not name the holder: %+v", liveness)
	}
	if !liveness.Beat || liveness.Overdue {
		t.Fatalf("a board beating twenty seconds ago read as silent or overdue: %+v", liveness)
	}
	// The one number a holder can act on: report before this or start
	// reading as overdue.
	if liveness.IntervalSeconds != defaultHolderHeartbeatInterval.Seconds() {
		t.Fatalf("the reporting interval was not the configured one: %+v", liveness)
	}
	if liveness.NextBeatBy == nil || liveness.LastSeenAt == nil ||
		!liveness.NextBeatBy.Equal(liveness.LastSeenAt.Add(board.HeartbeatGraceBeats*defaultHolderHeartbeatInterval)) {
		t.Fatalf("the next beat was not due a grace after the last one: %+v", liveness)
	}
	if liveness.ExpiresAt == nil || !liveness.ExpiresAt.Equal(f.board.Lease.ExpiresAt) {
		t.Fatalf("the report moved or dropped the expiry: %+v", liveness)
	}
}

func TestHeartbeatReportsSilenceWithoutEndingTheLease(t *testing.T) {
	f := &fakeBoardStore{board: heldBoard(time.Now().UTC().Add(-25 * time.Minute))}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", heartbeatPath(),
		`{"expected_version":41,"generation":7}`))
	if w.Code != http.StatusOK {
		t.Fatalf("a silent holder could not report: status=%d body=%s", w.Code, w.Body)
	}
	liveness := decodeLiveness(t, w.Body.Bytes())
	if !liveness.Overdue || liveness.SilenceSeconds < 1200 {
		t.Fatalf("twenty-five minutes of silence did not read as overdue: %+v", liveness)
	}
	// Overdue is evidence to report, never authority withdrawn: the lease
	// still runs to its own expiry and the response says so.
	if liveness.ExpiresAt == nil || !liveness.ExpiresAt.After(time.Now().UTC()) {
		t.Fatalf("an overdue holder was reported as out of time: %+v", liveness)
	}
	if liveness.Explain == "" {
		t.Fatal("an overdue holder was reported with no explanation")
	}
}

func TestHeartbeatValidatesTheFenceBeforeTouchingTheStore(t *testing.T) {
	for _, tc := range []struct{ name, path, body string }{
		{"no generation", heartbeatPath(), `{"expected_version":41,"generation":0}`},
		{"invalid lease", "/v1/boards/ek-ra8d2/leases/not-an-id/heartbeat", `{"expected_version":41,"generation":7}`},
		{"unknown field", heartbeatPath(), `{"expected_version":41,"generation":7,"expires_at":"2030-01-01T00:00:00Z"}`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := &fakeBoardStore{board: heldBoard(time.Time{})}
			w := httptest.NewRecorder()
			boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", tc.path, tc.body))
			if w.Code != http.StatusBadRequest || f.applies != 0 {
				t.Fatalf("an unfenced beat reached the store: status=%d applies=%d", w.Code, f.applies)
			}
		})
	}
}

func TestHeartbeatFromASupersededHolderIsRefused(t *testing.T) {
	f := &fakeBoardStore{board: heldBoard(time.Time{}),
		applyErr: &board.Error{Code: board.StaleGeneration, Detail: "lease token is stale"}}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", heartbeatPath(),
		`{"expected_version":41,"generation":6}`))
	if w.Code != http.StatusConflict {
		t.Fatalf("a stale beat was not refused: status=%d body=%s", w.Code, w.Body)
	}
}

func TestHeartbeatRequiresCertScopedGrantAndAuditsDenial(t *testing.T) {
	f := &fakeBoardStore{authorizeErr: store.ErrDenied, board: heldBoard(time.Time{})}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", heartbeatPath(),
		`{"expected_version":41,"generation":7}`))
	if w.Code != http.StatusNotFound || f.audits != 1 || f.action != "board.heartbeat" || f.applies != 0 {
		t.Fatalf("an unauthorized beat was not audited or was applied: status=%d audits=%d action=%q applies=%d",
			w.Code, f.audits, f.action, f.applies)
	}
}

func TestHeartbeatIntervalIsConfiguredWithinTheStateMachinesCeiling(t *testing.T) {
	if err := RegisterBoardRoutes(http.NewServeMux(), &fakeBoardStore{}, nil, "repo",
		BoardPolicy{HeartbeatInterval: board.MaxHeartbeatInterval + time.Second}); err == nil {
		t.Fatal("an interval past the ceiling was accepted")
	}
	if err := RegisterBoardRoutes(http.NewServeMux(), &fakeBoardStore{}, nil, "repo",
		BoardPolicy{HeartbeatInterval: -time.Second}); err == nil {
		t.Fatal("a negative interval was accepted")
	}
	mux := http.NewServeMux()
	f := &fakeBoardStore{board: heldBoard(time.Now().UTC().Add(-time.Second))}
	if err := RegisterBoardRoutes(mux, f, nil, "bsikar/ra8-firmware",
		BoardPolicy{HeartbeatInterval: board.MaxHeartbeatInterval}); err != nil {
		t.Fatalf("the ceiling itself was refused: %v", err)
	}
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, boardTestRequest("POST", heartbeatPath(), `{"expected_version":41,"generation":7}`))
	if w.Code != http.StatusOK {
		t.Fatalf("a beat under a configured interval failed: status=%d body=%s", w.Code, w.Body)
	}
	if liveness := decodeLiveness(t, w.Body.Bytes()); liveness.IntervalSeconds != board.MaxHeartbeatInterval.Seconds() {
		t.Fatalf("the configured interval was not the one reported: %+v", liveness)
	}
}

func TestHeartbeatOnAnUnheldBoardReportsNoHolder(t *testing.T) {
	free, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeBoardStore{board: free}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", heartbeatPath(),
		`{"expected_version":0,"generation":7}`))
	if w.Code != http.StatusOK {
		t.Fatalf("unexpected status for an unheld board: %d body=%s", w.Code, w.Body)
	}
	liveness := decodeLiveness(t, w.Body.Bytes())
	if liveness.Held || liveness.LastSeenAt != nil || liveness.NextBeatBy != nil || liveness.Overdue {
		t.Fatalf("a board nobody holds was reported as having a holder: %+v", liveness)
	}
}
