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

// The offline ingest door reads a record before any of it becomes durable
// history. repository_argument_test.go holds what it decides about a stated
// repository, and the_arguments_a_submitted_door_takes_test.go holds its
// content type. This takes the reading itself: what the door will parse,
// what it refuses, and the order those refusals happen in.

// grantingAuthorizer lets the request through and counts what it was asked,
// so a refusal after it is proof the door got past authorization and stopped
// on its own terms rather than on a denial.
type grantingAuthorizer struct{ asked []string }

func (a *grantingAuthorizer) Authorize(_ *http.Request, repository, _ string) (string, error) {
	a.asked = append(a.asked, repository)
	return "offline-principal", nil
}

// offlineRecord is the fixture entry as the door receives it: JSON text, so
// a test can bend the text itself rather than only the struct behind it.
func offlineRecord(t *testing.T) string {
	t.Helper()
	entry, _ := offlineTestEntry(t)
	raw, err := json.Marshal(entry)
	if err != nil {
		t.Fatalf("marshal entry: %v", err)
	}
	return string(raw)
}

// syncing posts a body at the offline door with an authorizer that would
// grant, and answers with the status, the problem detail and whether the
// authorizer was ever consulted.
func syncing(t *testing.T, body string) (int, string, int) {
	t.Helper()
	authorizer := &grantingAuthorizer{}
	api := &Server{auth: authorizer, audit: &recordingAuditor{}}

	request := httptest.NewRequest(http.MethodPost, "/v1/local-runs/sync", strings.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	api.ingestOffline(response, request)

	detail := ""
	if response.Body.Len() > 0 {
		var problem map[string]any
		if err := json.Unmarshal(response.Body.Bytes(), &problem); err == nil {
			detail, _ = problem["detail"].(string)
		}
	}
	return response.Code, detail, len(authorizer.asked)
}

// TestTheOfflineDoorRefusesTextItCannotRead takes the three ways the body
// itself is wrong. Each is refused before the authorizer is consulted, which
// is what keeps an unreadable record out of the denial audit trail.
func TestTheOfflineDoorRefusesTextItCannotRead(t *testing.T) {
	record := offlineRecord(t)

	for name, refused := range map[string]struct {
		body   string
		detail string
	}{
		"not JSON at all":    {body: "not json", detail: "invalid offline record"},
		"an empty body":      {body: "", detail: "invalid offline record"},
		"a JSON array":       {body: `[]`, detail: "invalid offline record"},
		"a bare string":      {body: `"record"`, detail: "invalid offline record"},
		"a field nobody has": {body: strings.TrimSuffix(record, "}") + `,"replayed":true}`, detail: "invalid offline record"},
		"a second record":    {body: record + record, detail: "trailing JSON data"},
		"trailing text":      {body: record + " {}", detail: "trailing JSON data"},
	} {
		status, detail, asked := syncing(t, refused.body)
		if status != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", name, status)
		}
		if detail != refused.detail {
			t.Fatalf("%s answered %q, want %q", name, detail, refused.detail)
		}
		if asked != 0 {
			t.Fatalf("%s was carried into an authorization", name)
		}
	}
}

// A record the decoder accepts can still be text the plane refuses to hash.
// A duplicate key is the case: encoding/json takes the last one and says
// nothing, so without the canonicalizing pass two different payloads would
// reach history under one digest. The refusal lands AFTER authorization and
// BEFORE the store is asked anything, which is the whole ordering this test
// exists to hold: the fixture server has no store at all, so reaching one
// would fault rather than answer 400.
func TestTheOfflineDoorRefusesARecordItCannotCanonicalize(t *testing.T) {
	record := offlineRecord(t)
	duplicated := strings.TrimSuffix(record, "}") + `,"id":"` + strings.Repeat("a", 32) + `"}`

	var into map[string]any
	if err := json.Unmarshal([]byte(duplicated), &into); err != nil {
		t.Fatalf("the duplicated record is not JSON an ordinary decoder takes: %v", err)
	}

	status, detail, asked := syncing(t, duplicated)
	if status != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", status)
	}
	if detail != "offline JSON cannot be canonicalized" {
		t.Fatalf("answered %q", detail)
	}
	if asked != 1 {
		t.Fatalf("the canonicalizing pass ran %d authorizations, want 1", asked)
	}
}

// The body bound is read off the reader, so a record past it is refused for
// its size rather than parsed and found wanting.
func TestTheOfflineDoorRefusesARecordPastItsBound(t *testing.T) {
	oversized := `{"id":"` + strings.Repeat("a", 256<<10) + `"}`

	status, detail, asked := syncing(t, oversized)
	if status != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", status)
	}
	if detail != "offline record exceeds limit or is unreadable" {
		t.Fatalf("answered %q", detail)
	}
	if asked != 0 {
		t.Fatalf("an oversized record was carried into an authorization")
	}
}

// A record stating no repository is refused on the repository, not on the
// task metadata behind it, so the operator is told the thing that is
// actually missing. The unusable cases live in repository_argument_test.go;
// this is the empty one, which is the shape a hand-written record takes.
func TestTheOfflineDoorNamesAMissingRepository(t *testing.T) {
	entry, _ := offlineTestEntry(t)
	entry.Source.Repository = ""
	raw, err := json.Marshal(entry)
	if err != nil {
		t.Fatalf("marshal entry: %v", err)
	}

	status, detail, asked := syncing(t, string(raw))
	if status != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", status)
	}
	if detail != "offline record states no usable repository" {
		t.Fatalf("answered %q", detail)
	}
	if asked != 0 {
		t.Fatalf("a record with no repository was carried into an authorization")
	}
}

// The door only reads history. A record that parses is still never a
// scheduled task, so the request carries no way to name one: the decoder
// refuses the fields a caller would use to try.
func TestTheOfflineDoorTakesNoInstructionToRunAnything(t *testing.T) {
	record := strings.TrimSuffix(offlineRecord(t), "}")

	for _, field := range []string{
		`,"trigger":"manual"}`,
		`,"tasks":["format-check"]}`,
		`,"principal_id":"someone-else"}`,
		`,"payload_sha256":"` + strings.Repeat("0", 64) + `"}`,
	} {
		status, detail, asked := syncing(t, record+field)
		if status != http.StatusBadRequest || detail != "invalid offline record" {
			t.Fatalf("%s answered %d %q", field, status, detail)
		}
		if asked != 0 {
			t.Fatalf("%s was carried into an authorization", field)
		}
	}
}

// The reader is bounded before it is read, not after: a body that lies about
// its length is refused rather than buffered whole.
func TestTheOfflineDoorBoundsTheBodyItReads(t *testing.T) {
	request := httptest.NewRequest(http.MethodPost, "/v1/local-runs/sync",
		bytes.NewReader(bytes.Repeat([]byte("x"), (256<<10)+1)))
	request.Header.Set("Content-Type", "application/json")
	authorizer := &grantingAuthorizer{}
	api := &Server{auth: authorizer, audit: &recordingAuditor{}}
	response := httptest.NewRecorder()

	api.ingestOffline(response, request)

	if response.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", response.Code)
	}
	if len(authorizer.asked) != 0 {
		t.Fatalf("an oversized body was carried into an authorization")
	}
}
