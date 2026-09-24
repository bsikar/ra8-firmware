package server

import (
	"context"
	"encoding/json"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"net/http"
	"net/http/httptest"
	"strings"
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

type fakeBoardHILClaims struct {
	*fakeBoardStore
	leaseID       string
	facts         store.StartAttemptInput
	catalogDigest string
	trustedCommit string
}

func (f *fakeBoardHILClaims) ClaimNextBoardHILAttempt(_ context.Context, _ store.BoardActor, leaseID string, facts store.StartAttemptInput, cat store.HILDefinitionCatalog, commit string) (*store.BoardHILAssignment, error) {
	f.leaseID, f.facts, f.catalogDigest, f.trustedCommit = leaseID, facts, cat.Digest(), commit
	return &store.BoardHILAssignment{Attempt: store.Attempt{ID: boardTestProofID, TaskID: boardTestLeaseID, AttemptNo: 1, State: "running"}}, nil
}

func TestBoardHILClaimRouteUsesLeaseAndHostFacts(t *testing.T) {
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeBoardHILClaims{fakeBoardStore: &fakeBoardStore{}}
	mux := http.NewServeMux()
	policy := BoardPolicy{Catalog: cat, TrustedCommit: strings.Repeat("a", 40)}
	if err := RegisterBoardRoutes(mux, f, nil, "bsikar/ra8-firmware", policy); err != nil {
		t.Fatal(err)
	}
	body, err := json.Marshal(claimHILAttemptRequest{LeaseID: boardTestLeaseID, Host: "ra8-board",
		HostCores: 8, HostRAMBytes: 8589934592, HostLoad: 0.25,
		HostFacts: json.RawMessage("{\"os\":\"linux\",\"arch\":\"amd64\"}")})
	if err != nil {
		t.Fatal(err)
	}
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/hil-attempts/claim", string(body)))
	if w.Code != http.StatusOK || f.leaseID != boardTestLeaseID ||
		f.facts.ActorID != "" || f.facts.Engine != "board-agent" ||
		f.facts.Host != "ra8-board" || f.facts.HostCores != 8 ||
		f.facts.HostRAMBytes != 8589934592 || f.catalogDigest != cat.Digest() ||
		f.trustedCommit != policy.TrustedCommit {
		t.Fatalf("HIL claim not policy/facts bound: status=%d fake=%+v body=%s", w.Code, f, w.Body.String())
	}
	var response struct {
		Assignment *store.BoardHILAssignment `json:"assignment"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &response); err != nil || response.Assignment == nil ||
		response.Assignment.Attempt.ID != boardTestProofID {
		t.Fatalf("claim response lost assignment: %+v err=%v body=%s", response, err, w.Body.String())
	}
}

type fakeBoardHILFinisher struct {
	*fakeBoardStore
	completion    store.BoardHILCompletion
	catalogDigest string
}

func (f *fakeBoardHILFinisher) CompleteBoardHILAttempt(_ context.Context, _ store.BoardActor,
	completion store.BoardHILCompletion, cat store.HILDefinitionCatalog, _ string) error {
	f.completion, f.catalogDigest = completion, cat.Digest()
	return nil
}

func TestBoardHILCompletionRouteBindsAttemptLeaseAndCatalog(t *testing.T) {
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeBoardHILFinisher{fakeBoardStore: &fakeBoardStore{}}
	mux := http.NewServeMux()
	if err := RegisterBoardRoutes(mux, f, nil, "bsikar/ra8-firmware", BoardPolicy{Catalog: cat}); err != nil {
		t.Fatal(err)
	}
	body := `{"lease_id":"` + boardTestLeaseID + `","generation":5,"result":"failed","hit_deadline":false,"evidence_complete":false,"reason":"fixture failure","steps":[{"key":"observe","started_at":"2026-01-01T00:00:00Z","ended_at":"2026-01-01T00:00:01Z","duration_ns":1000000000,"state":"failed","child_exit_code":1}]}`
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/hil-attempts/"+boardTestProofID+"/complete", body))
	if w.Code != http.StatusOK || f.completion.AttemptID != boardTestProofID ||
		f.completion.LeaseID != boardTestLeaseID || f.completion.Generation != 5 ||
		f.completion.Result != "failed" || len(f.completion.Steps) != 1 || f.catalogDigest != cat.Digest() {
		t.Fatalf("HIL completion was not bound to route identity and catalog: status=%d completion=%+v digest=%s body=%s",
			w.Code, f.completion, f.catalogDigest, w.Body.String())
	}
}
