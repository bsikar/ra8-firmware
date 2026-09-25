package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func decodeLivenessRead(t *testing.T, body []byte) (board.Snapshot, holderLivenessView) {
	t.Helper()
	var reply struct {
		Snapshot board.Snapshot     `json:"snapshot"`
		Liveness holderLivenessView `json:"liveness"`
	}
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("liveness reply did not decode: %v", err)
	}
	return reply.Snapshot, reply.Liveness
}

func TestLivenessReadReportsTheHolderWithoutBeatingForIt(t *testing.T) {
	seen := time.Now().UTC().Add(-20 * time.Second)
	f := &fakeBoardStore{board: heldBoard(seen)}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/ek-ra8d2/liveness", ""))
	if w.Code != http.StatusOK {
		t.Fatalf("the holder's liveness could not be read: status=%d body=%s", w.Code, w.Body)
	}
	// The whole point of a separate read: an operator asking whether a
	// holder is alive must not record a beat that says it is.
	if f.applies != 0 {
		t.Fatalf("reading liveness applied %d board commands", f.applies)
	}
	snapshot, liveness := decodeLivenessRead(t, w.Body.Bytes())
	if snapshot.BoardID != "ek-ra8d2" || snapshot.Version != f.board.Version {
		t.Fatalf("the read did not carry the board it judged: %+v", snapshot)
	}
	if !liveness.Held || liveness.Holder != "runner-3" || liveness.LeaseID != boardTestLeaseID {
		t.Fatalf("the report did not name the holder: %+v", liveness)
	}
	if !liveness.Beat || liveness.Overdue {
		t.Fatalf("a board beating twenty seconds ago read as silent or overdue: %+v", liveness)
	}
	if liveness.LastSeenAt == nil || !liveness.LastSeenAt.Equal(seen) {
		t.Fatalf("the report moved the beat it read: %+v", liveness)
	}
	// An operator and a holder must not read two different deadlines for
	// the same beat.
	if liveness.IntervalSeconds != defaultHolderHeartbeatInterval.Seconds() ||
		liveness.NextBeatBy == nil ||
		!liveness.NextBeatBy.Equal(seen.Add(board.HeartbeatGraceBeats*defaultHolderHeartbeatInterval)) {
		t.Fatalf("the read used a different reporting interval than the beat does: %+v", liveness)
	}
}

func TestLivenessReadReportsSilenceAndLeavesTheLeaseStanding(t *testing.T) {
	silent := time.Now().UTC().Add(-25 * time.Minute)
	f := &fakeBoardStore{board: heldBoard(silent)}
	expiry := f.board.Lease.ExpiresAt
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/ek-ra8d2/liveness", ""))
	if w.Code != http.StatusOK {
		t.Fatalf("a silent board could not be read: status=%d body=%s", w.Code, w.Body)
	}
	_, liveness := decodeLivenessRead(t, w.Body.Bytes())
	if !liveness.Overdue || liveness.SilenceSeconds < (20*time.Minute).Seconds() {
		t.Fatalf("twenty-five minutes of silence did not read as overdue: %+v", liveness)
	}
	// Overdue is a report, never a verdict: the lease it describes is
	// still the holder's, and still has an hour and a half to run.
	if f.applies != 0 || !f.board.Lease.ExpiresAt.Equal(expiry) || f.board.Phase != board.Active {
		t.Fatalf("reading silence changed the lease: applies=%d board=%+v", f.applies, f.board)
	}
	if liveness.ExpiresAt == nil || !liveness.ExpiresAt.Equal(expiry) || !liveness.ExpiresAt.After(time.Now().UTC()) {
		t.Fatalf("an overdue holder was reported as out of time: %+v", liveness)
	}
}

func TestLivenessReadOfAnUnheldBoardNamesNobody(t *testing.T) {
	free := heldBoard(time.Now().UTC())
	free.Phase, free.Lease = board.Ready, nil
	f := &fakeBoardStore{board: free}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/ek-ra8d2/liveness", ""))
	if w.Code != http.StatusOK {
		t.Fatalf("an idle board could not be read: status=%d body=%s", w.Code, w.Body)
	}
	_, liveness := decodeLivenessRead(t, w.Body.Bytes())
	if liveness.Held || liveness.Holder != "" || liveness.LeaseID != "" ||
		liveness.Beat || liveness.Overdue || liveness.LastSeenAt != nil ||
		liveness.NextBeatBy != nil || liveness.ExpiresAt != nil {
		t.Fatalf("an unheld board reported a holder: %+v", liveness)
	}
}

func TestLivenessReadRefusesAnInvalidBoardBeforeReadingOne(t *testing.T) {
	f := &fakeBoardStore{board: heldBoard(time.Now().UTC())}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/ek$ra8d2/liveness", ""))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("an invalid board ID was read: status=%d body=%s", w.Code, w.Body)
	}
}
