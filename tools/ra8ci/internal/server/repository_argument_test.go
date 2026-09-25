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

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// deniedAuthorizer refuses every request, which is what puts the caller's
// stated repository in front of the denial audit trail.
type deniedAuthorizer struct{ asked []string }

func (a *deniedAuthorizer) Authorize(_ *http.Request, repository, _ string) (string, error) {
	a.asked = append(a.asked, repository)
	return "", store.ErrDenied
}

func ingestRequest(t *testing.T, repository string) *http.Request {
	t.Helper()
	entry, _ := offlineTestEntry(t)
	entry.Source.Repository = repository
	body, err := json.Marshal(entry)
	if err != nil {
		t.Fatalf("marshal entry: %v", err)
	}
	request := httptest.NewRequest(http.MethodPost, "/v1/local-runs/sync", bytes.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	return request
}

func TestUsableRepositoryAcceptsARepositoryName(t *testing.T) {
	for _, repository := range []string{
		"bsikar/ra8-firmware",
		"a",
		strings.Repeat("r", maxRepositoryLength),
		"bsikar/ra8-firmware.git",
	} {
		if !usableRepository(repository) {
			t.Fatalf("usable repository %q was refused", repository)
		}
	}
}

func TestUsableRepositoryRefusesTextARepositoryCannotBe(t *testing.T) {
	for name, repository := range map[string]string{
		"none stated":               "",
		"over the bound":            strings.Repeat("r", maxRepositoryLength+1),
		"a whole payload":           strings.Repeat("r", 256<<10),
		"carries a line break":      "bsikar/ra8-firmware\ncertificate-sha256:0000 run.create everything",
		"carries a carriage return": "bsikar/ra8-firmware\rdenied",
		"carries a null":            "bsikar/ra8-firmware\x00",
		"carries an escape":         "bsikar/\x1b[2Kra8-firmware",
		"is not text at all":        "bsikar/\xff\xfe",
	} {
		if usableRepository(repository) {
			t.Fatalf("repository that %s was accepted", name)
		}
	}
}

// The length bound is the one the slow report already stated. Holding both
// handlers to one definition is the point of the rule living in its own file.
func TestUsableRepositoryKeepsTheLengthTheSlowReportAlwaysEnforced(t *testing.T) {
	if maxRepositoryLength != 512 {
		t.Fatalf("repository bound is %d, want the 512 the slow report enforced", maxRepositoryLength)
	}
}

func TestOfflineIngestRefusesAnUnusableRepositoryBeforeAuditingIt(t *testing.T) {
	authorizer := &deniedAuthorizer{}
	auditor := &recordingAuditor{}
	api := &Server{auth: authorizer, audit: auditor}
	response := httptest.NewRecorder()

	api.ingestOffline(response, ingestRequest(t, strings.Repeat("payload", 4096)))

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

func TestOfflineIngestKeepsALineBreakOutOfTheDenialAudit(t *testing.T) {
	authorizer := &deniedAuthorizer{}
	auditor := &recordingAuditor{}
	api := &Server{auth: authorizer, audit: auditor}
	response := httptest.NewRecorder()

	api.ingestOffline(response, ingestRequest(t, "bsikar/ra8-firmware\nunverified-peer local_run.ingest someone-elses-repo"))

	if response.Code != http.StatusBadRequest {
		t.Fatalf("repository carrying a line break answered %d, want 400", response.Code)
	}
	if len(auditor.records) != 0 {
		t.Fatalf("forged audit line was recorded: %+v", auditor.records)
	}
}

// A stated repository is still refused by the authorizer, and THAT denial is
// audited: the new check narrows what may be written, it does not stop the
// path from recording a real refusal.
func TestOfflineIngestStillAuditsADeniedStatedRepository(t *testing.T) {
	authorizer := &deniedAuthorizer{}
	auditor := &recordingAuditor{}
	api := &Server{auth: authorizer, audit: auditor}
	response := httptest.NewRecorder()

	api.ingestOffline(response, ingestRequest(t, "bsikar/ra8-firmware"))

	if response.Code != http.StatusNotFound {
		t.Fatalf("denied ingest answered %d, want 404", response.Code)
	}
	if len(auditor.records) != 1 || auditor.records[0].action != "local_run.ingest" ||
		auditor.records[0].target != "bsikar/ra8-firmware" {
		t.Fatalf("unexpected audit for a denied ingest: %+v", auditor.records)
	}
}

func TestSlowReportRefusesAnUnusableRepositoryBeforeAuditingIt(t *testing.T) {
	authorizer := &deniedAuthorizer{}
	auditor := &recordingAuditor{}
	api := &Server{auth: authorizer, audit: auditor}
	response := httptest.NewRecorder()
	query := "repository=" + strings.Repeat("r", maxRepositoryLength+1) + "&window_seconds=60&limit=10"

	api.slowReport(response, httptest.NewRequest(http.MethodGet, "/v1/reports/slow?"+query, nil))

	if response.Code != http.StatusBadRequest {
		t.Fatalf("over-long repository answered %d, want 400", response.Code)
	}
	if len(auditor.records) != 0 || len(authorizer.asked) != 0 {
		t.Fatalf("over-long repository was authorized or audited: %+v %+v", auditor.records, authorizer.asked)
	}
}
