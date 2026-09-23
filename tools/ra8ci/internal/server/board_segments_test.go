package server

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type fakeDurableSegments struct {
	*fakeBoardStore
	begun           store.BoardSegment
	beginToken      board.Token
	beginAttemptID  string
	beginBound      time.Duration
	beginMargin     time.Duration
	finishedID      string
	finishToken     board.Token
	finishAttemptID string
	finishOutcome   string
}

func (f *fakeDurableSegments) BeginBoardSegment(_ context.Context, _ store.BoardActor, version uint64, token board.Token, attemptID, key string, bound, margin time.Duration) (store.BoardSegment, error) {
	f.version, f.beginToken, f.beginAttemptID, f.beginBound, f.beginMargin = version, token, attemptID, bound, margin
	f.begun = store.BoardSegment{ID: boardTestProofID, BoardID: token.BoardID, LeaseID: token.LeaseID, Generation: token.Generation, AttemptID: attemptID, Key: key}
	return f.begun, nil
}

func (f *fakeDurableSegments) FinishBoardSegment(_ context.Context, _ store.BoardActor, id string, token board.Token, attemptID, outcome string) error {
	f.finishedID, f.finishToken, f.finishAttemptID, f.finishOutcome = id, token, attemptID, outcome
	return nil
}

func TestBoardSegmentRoutesBindLeaseAndBound(t *testing.T) {
	f := &fakeDurableSegments{fakeBoardStore: &fakeBoardStore{}}
	mux := http.NewServeMux()
	if err := RegisterBoardRoutes(mux, f, nil, "bsikar/ra8-firmware"); err != nil {
		t.Fatal(err)
	}
	body := `{"expected_version":12,"lease_id":"` + boardTestLeaseID + `","generation":5,"attempt_id":"01996f90-3415-7cfe-8ff1-600058131aff","key":"flash","bound_milliseconds":25000,"recovery_margin_ms":3000}`
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/segments/begin", body))
	if w.Code != http.StatusCreated || f.version != 12 || f.beginToken != (board.Token{BoardID: "ek-ra8d2", LeaseID: boardTestLeaseID, Generation: 5}) || f.beginAttemptID != "01996f90-3415-7cfe-8ff1-600058131aff" || f.beginBound != 25*time.Second || f.beginMargin != 3*time.Second {
		t.Fatalf("segment begin not fenced: status=%d version=%d token=%+v bound=%s margin=%s body=%s", w.Code, f.version, f.beginToken, f.beginBound, f.beginMargin, w.Body.String())
	}
	finish := `{"attempt_id":"01996f90-3415-7cfe-8ff1-600058131aff","lease_id":"` + boardTestLeaseID + `","generation":5,"outcome":"yielded"}`
	w = httptest.NewRecorder()
	mux.ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/segments/"+boardTestProofID+"/finish", finish))
	if w.Code != http.StatusOK || f.finishedID != boardTestProofID || f.finishToken != f.beginToken || f.finishAttemptID != "01996f90-3415-7cfe-8ff1-600058131aff" || f.finishOutcome != "yielded" {
		t.Fatalf("segment finish not fenced: status=%d id=%q token=%+v outcome=%q body=%s", w.Code, f.finishedID, f.finishToken, f.finishOutcome, w.Body.String())
	}
}

func TestBoardSegmentRejectsUnboundedRequest(t *testing.T) {
	f := &fakeDurableSegments{fakeBoardStore: &fakeBoardStore{}}
	mux := http.NewServeMux()
	if err := RegisterBoardRoutes(mux, f, nil, "bsikar/ra8-firmware"); err != nil {
		t.Fatal(err)
	}
	body := `{"attempt_id":"01996f90-3415-7cfe-8ff1-600058131aff","expected_version":1,"lease_id":"` + boardTestLeaseID + `","generation":1,"key":"flash","bound_milliseconds":0,"recovery_margin_ms":0}`
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/segments/begin", body))
	if w.Code != http.StatusBadRequest || f.beginBound != 0 {
		t.Fatalf("unbounded segment was accepted: status=%d bound=%s body=%s", w.Code, f.beginBound, w.Body.String())
	}
}
