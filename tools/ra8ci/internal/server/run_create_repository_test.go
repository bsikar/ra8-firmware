// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// createRunRequest builds a run submission whose only interesting field is the
// repository the caller states. Everything else is a shape createRun accepts,
// so the answer under test is always about the repository.
func createRunRequest(t *testing.T, repository string) *http.Request {
	t.Helper()
	body, err := json.Marshal(map[string]any{
		"trigger": "manual",
		"source": map[string]any{
			"repo":            repository,
			"branch":          "main",
			"commit":          strings.Repeat("a", 40),
			"snapshot_sha256": strings.Repeat("b", 64),
		},
		"catalog_digest": strings.Repeat("c", 64),
		"tasks":          []map[string]any{{"key": "build", "name": "build"}},
	})
	if err != nil {
		t.Fatalf("marshal run request: %v", err)
	}
	request := httptest.NewRequest(http.MethodPost, "/v1/runs", bytes.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Idempotency-Key", "run-create-repository")
	return request
}

// unusableRepositoriesOverJSON is every refused shape that survives a JSON
// round trip. Invalid UTF-8 is not here on purpose: a JSON decoder replaces
// those bytes with U+FFFD, so no handler can ever see them and the function
// test in repository_argument_test.go is where that shape belongs.
func unusableRepositoriesOverJSON() map[string]string {
	return map[string]string{
		"none stated":               "",
		"over the bound":            strings.Repeat("r", maxRepositoryLength+1),
		"a whole payload":           strings.Repeat("payload", 4096),
		"carries a line break":      "bsikar/ra8-firmware\ncertificate-sha256:0000 run.create everything",
		"carries a carriage return": "bsikar/ra8-firmware\rdenied",
		"carries a null":            "bsikar/ra8-firmware\x00",
		"carries an escape":         "bsikar/\x1b[2Kra8-firmware",
	}
}

func TestRunCreateRefusesAnUnusableRepositoryBeforeAuditingIt(t *testing.T) {
	authorizer := &deniedAuthorizer{}
	auditor := &recordingAuditor{}
	api := &Server{auth: authorizer, audit: auditor}
	response := httptest.NewRecorder()

	api.createRun(response, createRunRequest(t, strings.Repeat("payload", 4096)))

	if response.Code != http.StatusBadRequest {
		t.Fatalf("unusable repository answered %d, want 400", response.Code)
	}
	if len(auditor.records) != 0 {
		t.Fatalf("refused repository reached the audit trail: %+v", auditor.records)
	}
	if len(authorizer.asked) != 0 {
		t.Fatalf("refused repository was carried into an authorization: %+v", authorizer.asked)
	}
}

func TestRunCreateKeepsALineBreakOutOfTheDenialAudit(t *testing.T) {
	authorizer := &deniedAuthorizer{}
	auditor := &recordingAuditor{}
	api := &Server{auth: authorizer, audit: auditor}
	response := httptest.NewRecorder()

	api.createRun(response, createRunRequest(t, "bsikar/ra8-firmware\nunverified-peer run.create someone-elses-repo"))

	if response.Code != http.StatusBadRequest {
		t.Fatalf("repository carrying a line break answered %d, want 400", response.Code)
	}
	if len(auditor.records) != 0 {
		t.Fatalf("forged audit line was recorded: %+v", auditor.records)
	}
}

func TestRunCreateRefusesEveryUnusableRepositoryShape(t *testing.T) {
	for name, repository := range unusableRepositoriesOverJSON() {
		authorizer := &deniedAuthorizer{}
		auditor := &recordingAuditor{}
		api := &Server{auth: authorizer, audit: auditor}
		response := httptest.NewRecorder()

		api.createRun(response, createRunRequest(t, repository))

		if response.Code != http.StatusBadRequest {
			t.Fatalf("repository that %s answered %d, want 400", name, response.Code)
		}
		if len(auditor.records) != 0 || len(authorizer.asked) != 0 {
			t.Fatalf("repository that %s was authorized or audited: %+v %+v", name, auditor.records, authorizer.asked)
		}
	}
}

// The refusal is the server's own sentence. Handing the caller's text back in
// the problem detail would put it in the operator's terminal, which is the
// other place this rule keeps attacker-chosen text out of.
func TestRunCreateDoesNotQuoteTheRefusedRepository(t *testing.T) {
	repository := "bsikar/ra8-firmware\x1b[2Kdo-not-echo-me"
	api := &Server{auth: &deniedAuthorizer{}, audit: &recordingAuditor{}}
	response := httptest.NewRecorder()

	api.createRun(response, createRunRequest(t, repository))

	if strings.Contains(response.Body.String(), "do-not-echo-me") {
		t.Fatalf("refusal quoted the caller's repository: %s", response.Body.String())
	}
}

// A repository the rule accepts still reaches the authorizer exactly as it was
// stated: the check narrows what may be written, it does not rewrite what is
// asked about.
func TestRunCreateCarriesAUsableRepositoryToTheAuthorizerUnchanged(t *testing.T) {
	for _, repository := range []string{
		"bsikar/ra8-firmware",
		"a",
		strings.Repeat("r", maxRepositoryLength),
	} {
		authorizer := &deniedAuthorizer{}
		auditor := &recordingAuditor{}
		api := &Server{auth: authorizer, audit: auditor}
		response := httptest.NewRecorder()

		api.createRun(response, createRunRequest(t, repository))

		if response.Code != http.StatusNotFound {
			t.Fatalf("denied submission for %q answered %d, want 404", repository, response.Code)
		}
		if len(authorizer.asked) != 1 || authorizer.asked[0] != repository {
			t.Fatalf("authorizer was asked %+v, want exactly %q", authorizer.asked, repository)
		}
	}
}

// The bound is a boundary, not a range: the longest usable repository is
// submitted and the next byte is refused.
func TestRunCreateHoldsTheRepositoryBoundAtItsEdge(t *testing.T) {
	atBound := &deniedAuthorizer{}
	api := &Server{auth: atBound, audit: &recordingAuditor{}}
	api.createRun(httptest.NewRecorder(), createRunRequest(t, strings.Repeat("r", maxRepositoryLength)))
	if len(atBound.asked) != 1 {
		t.Fatalf("repository at the bound was not authorized: %+v", atBound.asked)
	}

	pastBound := &deniedAuthorizer{}
	response := httptest.NewRecorder()
	api = &Server{auth: pastBound, audit: &recordingAuditor{}}
	api.createRun(response, createRunRequest(t, strings.Repeat("r", maxRepositoryLength+1)))
	if response.Code != http.StatusBadRequest || len(pastBound.asked) != 0 {
		t.Fatalf("one byte past the bound answered %d and asked %+v, want 400 and nothing asked", response.Code, pastBound.asked)
	}
}

// A stated repository is still refused by the authorizer, and THAT denial is
// audited, under the run-creation action and with the repository as target.
func TestRunCreateStillAuditsADeniedStatedRepository(t *testing.T) {
	auditor := &recordingAuditor{}
	api := &Server{auth: &deniedAuthorizer{}, audit: auditor}
	response := httptest.NewRecorder()

	api.createRun(response, createRunRequest(t, "bsikar/ra8-firmware"))

	if response.Code != http.StatusNotFound {
		t.Fatalf("denied submission answered %d, want 404", response.Code)
	}
	if len(auditor.records) != 1 || auditor.records[0].action != "run.create" ||
		auditor.records[0].target != "bsikar/ra8-firmware" {
		t.Fatalf("unexpected audit for a denied submission: %+v", auditor.records)
	}
}

// The check sits ahead of everything that reads server state, so a submission
// naming an unusable repository is refused without the catalog being consulted
// at all. The nil catalog is the assertion: reaching the digest comparison
// would not answer 400.
func TestRunCreateRefusesTheRepositoryBeforeReadingAnyServerState(t *testing.T) {
	api := &Server{auth: &deniedAuthorizer{}, audit: &recordingAuditor{}, catalog: nil}
	response := httptest.NewRecorder()

	api.createRun(response, createRunRequest(t, strings.Repeat("r", maxRepositoryLength+1)))

	if response.Code != http.StatusBadRequest {
		t.Fatalf("unusable repository answered %d, want 400", response.Code)
	}
}

// Every door that takes a caller-stated repository reads one definition of
// what a repository may be. This is the guard the rule was factored out for:
// a fourth handler that forgets it fails here.
func TestEveryCallerStatedRepositoryDoorRefusesTheSameText(t *testing.T) {
	overLong := strings.Repeat("r", maxRepositoryLength+1)
	doors := map[string]struct {
		handle  func(*Server, http.ResponseWriter, *http.Request)
		request func(*testing.T) *http.Request
	}{
		"run creation": {
			handle:  func(s *Server, w http.ResponseWriter, r *http.Request) { s.createRun(w, r) },
			request: func(t *testing.T) *http.Request { return createRunRequest(t, overLong) },
		},
		"offline ingest": {
			handle:  func(s *Server, w http.ResponseWriter, r *http.Request) { s.ingestOffline(w, r) },
			request: func(t *testing.T) *http.Request { return ingestRequest(t, overLong) },
		},
		"slow report": {
			handle: func(s *Server, w http.ResponseWriter, r *http.Request) { s.slowReport(w, r) },
			request: func(_ *testing.T) *http.Request {
				return httptest.NewRequest(http.MethodGet,
					"/v1/reports/slow?repository="+overLong+"&window_seconds=60&limit=10", nil)
			},
		},
	}
	for name, door := range doors {
		authorizer := &deniedAuthorizer{}
		auditor := &recordingAuditor{}
		api := &Server{auth: authorizer, audit: auditor}
		response := httptest.NewRecorder()

		door.handle(api, response, door.request(t))

		if response.Code != http.StatusBadRequest {
			t.Fatalf("%s answered %d for an over-long repository, want 400", name, response.Code)
		}
		if len(auditor.records) != 0 || len(authorizer.asked) != 0 {
			t.Fatalf("%s authorized or audited an over-long repository: %+v %+v", name, auditor.records, authorizer.asked)
		}
	}
}
