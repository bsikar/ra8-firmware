// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The two lease-bound HIL doors: a board agent claims the next attempt and
// later says how it went. Both hold their whole argument front before the
// store is asked anything, and both treat the store's HIL capability as
// optional, so a plane that cannot dispatch says so rather than failing
// somewhere deeper. None of this needs a database.

const hilTrustedCommit = "0123456789abcdef0123456789abcdef01234567"

// hilAttemptStore is the fake board store with the two optional HIL
// capabilities added, and it records what it was handed so the door can be
// held to passing the request down unchanged.
type hilAttemptStore struct {
	*fakeBoardStore
	assignment  *store.BoardHILAssignment
	claimErr    error
	completeErr error
	claims      int
	completes   int
	claimLease  string
	claimInput  store.StartAttemptInput
	completion  store.BoardHILCompletion
	commit      string
}

func (s *hilAttemptStore) ClaimNextBoardHILAttempt(_ context.Context, _ store.BoardActor, lease string, input store.StartAttemptInput, _ store.HILDefinitionCatalog, commit string) (*store.BoardHILAssignment, error) {
	s.claims++
	s.claimLease, s.claimInput, s.commit = lease, input, commit
	return s.assignment, s.claimErr
}

func (s *hilAttemptStore) CompleteBoardHILAttempt(_ context.Context, _ store.BoardActor, completion store.BoardHILCompletion, _ store.HILDefinitionCatalog, commit string) error {
	s.completes++
	s.completion, s.commit = completion, commit
	return s.completeErr
}

func newHILAttemptStore() *hilAttemptStore {
	return &hilAttemptStore{fakeBoardStore: &fakeBoardStore{}}
}

func hilAttemptPlane(t *testing.T, st BoardStore, cat *catalog.Catalog, commit string) *http.ServeMux {
	t.Helper()
	mux := http.NewServeMux()
	policy := BoardPolicy{Catalog: cat, TrustedCommit: commit}
	if err := RegisterBoardRoutes(mux, st, nil, "bsikar/ra8-firmware", policy); err != nil {
		t.Fatal(err)
	}
	return mux
}

func askedOfTheBoard(t *testing.T, mux *http.ServeMux, path, contentType, body string) answered {
	t.Helper()
	request := boardTestRequest(http.MethodPost, path, body)
	request.Header.Del("Content-Type")
	if contentType != "" {
		request.Header.Set("Content-Type", contentType)
	}
	response := httptest.NewRecorder()
	mux.ServeHTTP(response, request)

	result := answered{status: response.Code}
	if response.Body.Len() > 0 {
		if err := json.Unmarshal(response.Body.Bytes(), &result.body); err != nil {
			t.Fatalf("%s answered a body that is not JSON: %q", path, response.Body.String())
		}
	}
	return result
}

func completionPath(attemptID string) string {
	return "/v1/boards/ek-ra8d2/hil-attempts/" + attemptID + "/complete"
}

const claimPath = "/v1/boards/ek-ra8d2/hil-attempts/claim"

func aClaim(lease string) string {
	return `{"lease_id":"` + lease + `","host":"bench-1","host_cores":4,` +
		`"host_ram_bytes":17179869184,"host_load":0.5,"host_facts":{"os":"linux"}}`
}

// TestHILCompletionNeedsAStoreThatKeepsAttempts pins the optional-capability
// check. A plane whose store cannot finish an attempt, or that has no
// catalog to judge the task against, says the completion is not configured
// rather than taking the report and dropping it.
func TestHILCompletionNeedsAStoreThatKeepsAttempts(t *testing.T) {
	cat := reviewedCatalog(t)

	noCapability := askedOfTheBoard(t, hilAttemptPlane(t, &fakeBoardStore{}, cat, hilTrustedCommit),
		completionPath(boardTestProofID), "application/json", `{}`)
	if noCapability.status != http.StatusServiceUnavailable ||
		noCapability.body["detail"] != "lease-bound HIL completion is not configured" {
		t.Fatalf("a store with no HIL finisher answered %d %+v", noCapability.status, noCapability.body)
	}

	noCatalog := askedOfTheBoard(t, hilAttemptPlane(t, newHILAttemptStore(), nil, hilTrustedCommit),
		completionPath(boardTestProofID), "application/json", `{}`)
	if noCatalog.status != http.StatusServiceUnavailable ||
		noCatalog.body["detail"] != "lease-bound HIL completion is not configured" {
		t.Fatalf("a plane with no catalog answered %d %+v", noCatalog.status, noCatalog.body)
	}
}

// TestHILCompletionJudgesEveryArgumentBeforeTheStore walks the front of the
// door. The store is the assertion: it is never asked anything, so a report
// that is refused here was refused on its own terms.
func TestHILCompletionJudgesEveryArgumentBeforeTheStore(t *testing.T) {
	cat := reviewedCatalog(t)
	good := `{"lease_id":"` + boardTestProofID + `","generation":3,"result":"passed","steps":[]}`

	for name, refused := range map[string]struct {
		attemptID   string
		contentType string
		body        string
		status      int
		detail      string
	}{
		"an attempt ID that is not one": {
			attemptID: "not-an-attempt", contentType: "application/json", body: good,
			status: http.StatusBadRequest, detail: "invalid HIL attempt ID",
		},
		"no content type": {
			attemptID: boardTestProofID, body: good,
			status: http.StatusUnsupportedMediaType, detail: "content type must be application/json",
		},
		"a report that does not parse": {
			attemptID: boardTestProofID, contentType: "application/json", body: `{"result":`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a field this door does not know": {
			attemptID: boardTestProofID, contentType: "application/json", body: `{"result":"passed","notes":"fine"}`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a second report after the first": {
			attemptID: boardTestProofID, contentType: "application/json", body: good + ` {"result":"failed"}`,
			status: http.StatusBadRequest, detail: "trailing board request data",
		},
		"no lease at all": {
			attemptID: boardTestProofID, contentType: "application/json", body: `{"generation":3,"result":"passed"}`,
			status: http.StatusBadRequest, detail: "invalid HIL lease identity",
		},
		"a lease that is not an ID": {
			attemptID: boardTestProofID, contentType: "application/json", body: `{"lease_id":"lease-1","generation":3}`,
			status: http.StatusBadRequest, detail: "invalid HIL lease identity",
		},
		"generation zero": {
			attemptID: boardTestProofID, contentType: "application/json",
			body:   `{"lease_id":"` + boardTestProofID + `","generation":0,"result":"passed"}`,
			status: http.StatusBadRequest, detail: "invalid HIL lease identity",
		},
	} {
		st := newHILAttemptStore()
		result := askedOfTheBoard(t, hilAttemptPlane(t, st, cat, hilTrustedCommit),
			completionPath(refused.attemptID), refused.contentType, refused.body)
		if result.status != refused.status {
			t.Fatalf("%s: status = %d, want %d", name, result.status, refused.status)
		}
		if result.body["detail"] != refused.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, refused.detail)
		}
		if st.completes != 0 {
			t.Fatalf("%s was carried into the store", name)
		}
	}
}

// And the far side: a report that holds is handed down whole, with the
// attempt taken from the path rather than the body, and the plane's own
// trusted commit rather than anything the caller said.
func TestHILCompletionHandsTheStoreTheReportItWasGiven(t *testing.T) {
	st := newHILAttemptStore()
	exit := 0
	body, err := json.Marshal(map[string]any{
		"lease_id": boardTestProofID, "generation": 7, "result": "passed",
		"child_exit_code": exit, "hit_deadline": false, "evidence_complete": true,
		"reason": "", "steps": []store.HILStep{{Key: "flash", State: "passed"}},
	})
	if err != nil {
		t.Fatal(err)
	}

	result := askedOfTheBoard(t, hilAttemptPlane(t, st, reviewedCatalog(t), hilTrustedCommit),
		completionPath(boardTestProofID), "application/json", string(body))

	if result.status != http.StatusOK {
		t.Fatalf("status = %d, want 200: %+v", result.status, result.body)
	}
	if st.completes != 1 {
		t.Fatalf("the store was asked %d times, want once", st.completes)
	}
	if st.completion.AttemptID != boardTestProofID || st.completion.LeaseID != boardTestProofID ||
		st.completion.Generation != 7 || st.completion.Result != "passed" ||
		!st.completion.EvidenceComplete || len(st.completion.Steps) != 1 {
		t.Fatalf("the store was handed %+v", st.completion)
	}
	if st.completion.ChildExitCode == nil || *st.completion.ChildExitCode != 0 {
		t.Fatalf("a zero exit code did not survive the trip: %+v", st.completion.ChildExitCode)
	}
	if st.commit != hilTrustedCommit {
		t.Fatalf("the store was told commit %q, want the plane's own %q", st.commit, hilTrustedCommit)
	}
	if result.body["attempt_id"] != boardTestProofID || result.body["result"] != "passed" {
		t.Fatalf("answered %+v", result.body)
	}
}

// A store that refuses the report is reported in the board's own wording, so
// a denied agent reads the same 404 everywhere on this plane rather than
// learning that the attempt exists.
func TestHILCompletionReportsAStoresRefusalInTheBoardsWording(t *testing.T) {
	good := `{"lease_id":"` + boardTestProofID + `","generation":3,"result":"passed"}`

	for name, refusal := range map[string]struct {
		err    error
		status int
		detail string
	}{
		"a denied agent":          {err: store.ErrDenied, status: http.StatusNotFound, detail: "board not found or access denied"},
		"an attempt that is gone": {err: store.ErrNotFound, status: http.StatusNotFound, detail: "board not found"},
		"a store that is down":    {err: store.ErrUnavailable, status: http.StatusServiceUnavailable},
	} {
		st := newHILAttemptStore()
		st.completeErr = refusal.err
		result := askedOfTheBoard(t, hilAttemptPlane(t, st, reviewedCatalog(t), hilTrustedCommit),
			completionPath(boardTestProofID), "application/json", good)
		if result.status != refusal.status {
			t.Fatalf("%s: status = %d, want %d", name, result.status, refusal.status)
		}
		if refusal.detail != "" && result.body["detail"] != refusal.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, refusal.detail)
		}
	}
}

// TestHILClaimNeedsDispatchConfigured pins the three things a plane needs
// before it will hand out an attempt. The trusted commit is the one worth
// naming: a plane that does not know which commit it trusts will not
// dispatch HIL work at all, which is the whole point of pinning the source
// a board runs from.
func TestHILClaimNeedsDispatchConfigured(t *testing.T) {
	cat := reviewedCatalog(t)

	for name, plane := range map[string]*http.ServeMux{
		"a store that cannot dispatch":    hilAttemptPlane(t, &fakeBoardStore{}, cat, hilTrustedCommit),
		"a plane with no catalog":         hilAttemptPlane(t, newHILAttemptStore(), nil, hilTrustedCommit),
		"a plane with no trusted commit":  hilAttemptPlane(t, newHILAttemptStore(), cat, ""),
		"a plane whose commit is not one": hilAttemptPlane(t, newHILAttemptStore(), cat, strings.Repeat("z", 40)),
		"a plane whose commit is shouted": hilAttemptPlane(t, newHILAttemptStore(), cat, strings.ToUpper(hilTrustedCommit)),
	} {
		result := askedOfTheBoard(t, plane, claimPath, "application/json", aClaim(boardTestProofID))
		if result.status != http.StatusServiceUnavailable {
			t.Fatalf("%s: status = %d, want 503", name, result.status)
		}
		if result.body["detail"] != "lease-aware HIL dispatch is not configured" {
			t.Fatalf("%s answered %+v", name, result.body)
		}
	}
}

// TestHILClaimJudgesTheHostItWasToldAbout walks the claim's own arguments.
// Absent host facts are refused, because a scheduler that is told nothing
// about the host cannot honour what a task asks of one. The neighbouring
// json.Valid guard on those facts is belt and braces and is deliberately not
// pinned here: the decoder has already refused anything that is not JSON
// before the guard is reached, so no request can reach it.
func TestHILClaimJudgesTheHostItWasToldAbout(t *testing.T) {
	cat := reviewedCatalog(t)

	for name, refused := range map[string]struct {
		contentType string
		body        string
		status      int
		detail      string
	}{
		"no content type": {
			body: aClaim(boardTestProofID), status: http.StatusUnsupportedMediaType,
			detail: "content type must be application/json",
		},
		"a claim that does not parse": {
			contentType: "application/json", body: `{"host":`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a field this door does not know": {
			contentType: "application/json", body: `{"host":"bench-1","rack":"2"}`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a lease that is not an ID": {
			contentType: "application/json", body: aClaim("lease-1"),
			status: http.StatusBadRequest, detail: "invalid HIL claim request",
		},
		"a host with no name": {
			contentType: "application/json",
			body:        `{"lease_id":"` + boardTestProofID + `","host":"","host_cores":4,"host_ram_bytes":1,"host_load":0,"host_facts":{}}`,
			status:      http.StatusBadRequest, detail: "invalid HIL claim request",
		},
		"a host with no cores": {
			contentType: "application/json",
			body:        `{"lease_id":"` + boardTestProofID + `","host":"bench-1","host_cores":0,"host_ram_bytes":1,"host_load":0,"host_facts":{}}`,
			status:      http.StatusBadRequest, detail: "invalid HIL claim request",
		},
		"a host with no memory": {
			contentType: "application/json",
			body:        `{"lease_id":"` + boardTestProofID + `","host":"bench-1","host_cores":4,"host_ram_bytes":0,"host_load":0,"host_facts":{}}`,
			status:      http.StatusBadRequest, detail: "invalid HIL claim request",
		},
		"a negative load": {
			contentType: "application/json",
			body:        `{"lease_id":"` + boardTestProofID + `","host":"bench-1","host_cores":4,"host_ram_bytes":1,"host_load":-0.1,"host_facts":{}}`,
			status:      http.StatusBadRequest, detail: "invalid HIL claim request",
		},
		"no host facts at all": {
			contentType: "application/json",
			body:        `{"lease_id":"` + boardTestProofID + `","host":"bench-1","host_cores":4,"host_ram_bytes":1,"host_load":0}`,
			status:      http.StatusBadRequest, detail: "invalid HIL claim request",
		},
	} {
		st := newHILAttemptStore()
		result := askedOfTheBoard(t, hilAttemptPlane(t, st, cat, hilTrustedCommit), claimPath, refused.contentType, refused.body)
		if result.status != refused.status {
			t.Fatalf("%s: status = %d, want %d", name, result.status, refused.status)
		}
		if result.body["detail"] != refused.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, refused.detail)
		}
		if st.claims != 0 {
			t.Fatalf("%s was carried into the store", name)
		}
	}
}

// An empty queue is an answer, not an error: the agent is told there is no
// assignment and goes back to waiting. The engine the store is told about is
// the plane's own, not the caller's, so an agent cannot claim work as
// something else.
func TestHILClaimAnswersAnEmptyQueueWithoutAnAssignment(t *testing.T) {
	st := newHILAttemptStore()
	result := askedOfTheBoard(t, hilAttemptPlane(t, st, reviewedCatalog(t), hilTrustedCommit),
		claimPath, "application/json", aClaim(boardTestProofID))

	if result.status != http.StatusOK {
		t.Fatalf("status = %d, want 200: %+v", result.status, result.body)
	}
	if assignment, carried := result.body["assignment"]; !carried || assignment != nil {
		t.Fatalf("an empty queue answered %+v", result.body)
	}
	if st.claims != 1 || st.claimLease != boardTestProofID {
		t.Fatalf("the store was asked %d times for lease %q", st.claims, st.claimLease)
	}
	if st.claimInput.Engine != "board-agent" {
		t.Fatalf("the store was told engine %q, want the plane's own", st.claimInput.Engine)
	}
	if st.claimInput.Host != "bench-1" || st.claimInput.HostCores != 4 || st.claimInput.HostRAMBytes != 17179869184 {
		t.Fatalf("the host did not survive the trip: %+v", st.claimInput)
	}
	if st.commit != hilTrustedCommit {
		t.Fatalf("the store was told commit %q", st.commit)
	}
}

func TestHILClaimCarriesTheAssignmentItWasGiven(t *testing.T) {
	st := newHILAttemptStore()
	st.assignment = &store.BoardHILAssignment{RunID: boardTestProofID, Repository: "bsikar/ra8-firmware", Branch: "main"}

	result := askedOfTheBoard(t, hilAttemptPlane(t, st, reviewedCatalog(t), hilTrustedCommit),
		claimPath, "application/json", aClaim(boardTestProofID))

	if result.status != http.StatusOK {
		t.Fatalf("status = %d, want 200", result.status)
	}
	assignment, carried := result.body["assignment"].(map[string]any)
	if !carried {
		t.Fatalf("the assignment did not reach the agent: %+v", result.body)
	}
	if assignment["run_id"] != boardTestProofID || assignment["repository"] != "bsikar/ra8-firmware" {
		t.Fatalf("the assignment was rewritten on the way out: %+v", assignment)
	}
}

func TestHILClaimReportsAStoresRefusalInTheBoardsWording(t *testing.T) {
	for name, refusal := range map[string]struct {
		err    error
		status int
	}{
		"a denied agent":       {err: store.ErrDenied, status: http.StatusNotFound},
		"a board that is gone": {err: store.ErrNotFound, status: http.StatusNotFound},
		"a store that is down": {err: store.ErrUnavailable, status: http.StatusServiceUnavailable},
	} {
		st := newHILAttemptStore()
		st.claimErr = refusal.err
		result := askedOfTheBoard(t, hilAttemptPlane(t, st, reviewedCatalog(t), hilTrustedCommit),
			claimPath, "application/json", aClaim(boardTestProofID))
		if result.status != refusal.status {
			t.Fatalf("%s: status = %d, want %d", name, result.status, refusal.status)
		}
	}
}
