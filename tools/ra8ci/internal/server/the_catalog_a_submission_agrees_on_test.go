// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// the_arguments_a_submitted_door_takes_test.go pins everything run creation
// judges BEFORE it authorizes, with a plane whose authorizer refuses. This
// takes the other side of that line: the two decisions the door makes after
// authorization has already succeeded and before a run is written. Both are
// reached with a plane that holds no store at all, so a 409 or a 400 here is
// also proof the door stopped short of writing anything.

// submittingPlane authorizes every request and counts what it was asked, so
// a refusal after it can be told apart from a denial.
func submittingPlane(t *testing.T) (*Server, *grantingAuthorizer) {
	t.Helper()
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	authorizer := &grantingAuthorizer{}
	return &Server{audit: &recordingAuditor{}, catalog: cat, auth: authorizer}, authorizer
}

// creating posts a run submission and answers with the status, the problem
// detail, and how many authorizations the door spent getting there.
func creating(t *testing.T, body string) (int, string, int) {
	t.Helper()
	api, authorizer := submittingPlane(t)
	request := httptest.NewRequest(http.MethodPost, "/v1/runs", strings.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Idempotency-Key", "k")
	response := httptest.NewRecorder()

	api.createRun(response, request)

	detail := ""
	if response.Body.Len() > 0 {
		var problem map[string]any
		if err := json.Unmarshal(response.Body.Bytes(), &problem); err == nil {
			detail, _ = problem["detail"].(string)
		}
	}
	return response.Code, detail, len(authorizer.asked)
}

// aSubmission writes a run request whose catalog digest and task list are the
// parts under test. The repository spelling is "repo", which is what
// createRequest declares.
func aSubmission(digest, tasks string) string {
	return `{"trigger":"manual","catalog_digest":"` + digest +
		`","source":{"repo":"bsikar/ra8-firmware","branch":"main","commit":"` +
		strings.Repeat("a", 40) + `"},"tasks":[` + tasks + `]}`
}

func serverCatalogDigest(t *testing.T) string {
	t.Helper()
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	return cat.Digest()
}

// A submitter whose catalog differs from the server's is told so, with 409
// rather than 400: the request is well formed, the two sides just disagree
// about which tasks exist. The check sits AFTER authorization, which is why
// the authorization is spent before the mismatch is found.
func TestARunSubmissionMustAgreeOnTheCatalog(t *testing.T) {
	status, detail, asked := creating(t, aSubmission(strings.Repeat("c", 64), `{"key":"a","name":"format-check"}`))

	if status != http.StatusConflict {
		t.Fatalf("status = %d, want 409", status)
	}
	if detail != "task catalog digest differs from server" {
		t.Fatalf("answered %q", detail)
	}
	if asked != 1 {
		t.Fatalf("the digest was judged after %d authorizations, want 1", asked)
	}
}

// An empty digest is a mismatch like any other, not a request to skip the
// check. A door that read "" as "no opinion" would accept a submission built
// against a catalog nobody can name.
func TestARunSubmissionCannotDeclineToNameACatalog(t *testing.T) {
	for name, digest := range map[string]string{
		"no digest at all":     "",
		"a digest of the text": "catalog",
		"the digest shouted":   strings.ToUpper(serverCatalogDigest(t)),
		"one byte short":       serverCatalogDigest(t)[:63],
	} {
		status, detail, _ := creating(t, aSubmission(digest, `{"key":"a","name":"format-check"}`))
		if status != http.StatusConflict || detail != "task catalog digest differs from server" {
			t.Fatalf("%s answered %d %q", name, status, detail)
		}
	}
}

// With the catalogs agreed, every task named is looked up in the reviewed
// catalog. A name nobody reviewed is refused, and the refusal does not
// distinguish an unknown task from bad arguments: the submitter learns their
// task list is wrong, not which reviewed tasks exist.
func TestARunSubmissionOnlyNamesReviewedTasks(t *testing.T) {
	digest := serverCatalogDigest(t)

	for name, tasks := range map[string]string{
		"a task nobody reviewed": `{"key":"a","name":"rm-rf"}`,
		"a task with no name":    `{"key":"a","name":""}`,
		"a near miss":            `{"key":"a","name":"format_check"}`,
		"a shouted task":         `{"key":"a","name":"FORMAT-CHECK"}`,
		"a reviewed task second": `{"key":"a","name":"format-check"},{"key":"b","name":"rm-rf"}`,
	} {
		status, detail, asked := creating(t, aSubmission(digest, tasks))
		if status != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", name, status)
		}
		if detail != "unknown task or invalid task arguments" {
			t.Fatalf("%s answered %q", name, detail)
		}
		if asked != 1 {
			t.Fatalf("%s spent %d authorizations", name, asked)
		}
	}
}

// A task's argv is not the submitter's to state. The door binds arguments
// from the reviewed definition and the named values, then requires the
// submitted argv to be exactly what that binding produced, so a caller
// cannot smuggle a flag past a task the catalog reviewed without it.
func TestARunSubmissionCannotStateItsOwnArgv(t *testing.T) {
	digest := serverCatalogDigest(t)

	for name, tasks := range map[string]string{
		"an argument the binding does not make": `{"key":"a","name":"format-check","args":["--fix"]}`,
		"a flag with a value beside it":         `{"key":"a","name":"format-check","args":["--root","/"]}`,
		"a value nobody declared":               `{"key":"a","name":"format-check","values":{"root":"/"}}`,
		"a positional left unstated":            `{"key":"a","name":"ascii-rewrite"}`,
	} {
		status, detail, asked := creating(t, aSubmission(digest, tasks))
		if status != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", name, status)
		}
		if detail != "unknown task or invalid task arguments" {
			t.Fatalf("%s answered %q", name, detail)
		}
		if asked != 1 {
			t.Fatalf("%s spent %d authorizations", name, asked)
		}
	}
}

// The task list is read in order, so the first task that does not hold is
// the one that stops the submission. Pinned by putting a reviewed task
// before and after a refused one: either position is refused, and neither
// reaches a write.
func TestARunSubmissionIsRefusedOnItsFirstBadTask(t *testing.T) {
	digest := serverCatalogDigest(t)
	reviewed := `{"key":"a","name":"format-check"}`
	refused := `{"key":"b","name":"format-check","args":["--fix"]}`

	for name, tasks := range map[string]string{
		"the bad task first": refused + "," + reviewed,
		"the bad task last":  reviewed + "," + refused,
	} {
		status, detail, _ := creating(t, aSubmission(digest, tasks))
		if status != http.StatusBadRequest || detail != "unknown task or invalid task arguments" {
			t.Fatalf("%s answered %d %q", name, status, detail)
		}
	}
}
