// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	testRunID     = "00000000-0000-7000-8000-000000000001"
	testAttemptID = "00000000-0000-7000-8000-000000000002"
)

type fakeRunLogReader struct {
	page       store.LogPage
	lookups    int
	pageCalls  int
	pageRunID  string
	pageTryID  string
	pageAfter  int64
	pageLimit  int
	repository string
}

func (f *fakeRunLogReader) LookupRunRepository(_ context.Context, _ string) (string, error) {
	f.lookups++
	return f.repository, nil
}

func (f *fakeRunLogReader) AttemptLogs(_ context.Context, runID, attemptID string, after int64, limit int) (store.LogPage, error) {
	f.pageCalls++
	f.pageRunID, f.pageTryID, f.pageAfter, f.pageLimit = runID, attemptID, after, limit
	return f.page, nil
}

type allowRunLogRead struct{ calls int }

func (a *allowRunLogRead) Authorize(_ *http.Request, repository, permission string) (string, error) {
	a.calls++
	if repository != "bsikar/ra8-firmware" || permission != "read" {
		return "", store.ErrDenied
	}
	return "reader", nil
}

func TestRunLogsAuthorizesRunAndScopesAttemptPage(t *testing.T) {
	reader := &fakeRunLogReader{repository: "bsikar/ra8-firmware", page: store.LogPage{
		AttemptID: testAttemptID, NextAfter: 3, Chunks: []store.LogRecord{{Sequence: 3}},
	}}
	auth := &allowRunLogRead{}
	api := &Server{logReader: reader, auth: auth}
	request := httptest.NewRequest(http.MethodGet, "/v1/runs/"+testRunID+"/logs?attempt_id="+testAttemptID+"&after=2&limit=1", nil)
	request.SetPathValue("id", testRunID)
	response := httptest.NewRecorder()
	api.getRunLogs(response, request)
	if response.Code != http.StatusOK || auth.calls != 1 || reader.lookups != 1 || reader.pageCalls != 1 {
		t.Fatalf("status=%d auth=%d lookup=%d page=%d body=%s", response.Code, auth.calls, reader.lookups, reader.pageCalls, response.Body.String())
	}
	if reader.pageRunID != testRunID || reader.pageTryID != testAttemptID || reader.pageAfter != 2 || reader.pageLimit != 1 {
		t.Fatalf("store query mismatch: %+v", reader)
	}
}

func TestRunLogsRejectsInvalidQueryBeforeLookup(t *testing.T) {
	reader := &fakeRunLogReader{repository: "bsikar/ra8-firmware"}
	auth := &allowRunLogRead{}
	api := &Server{logReader: reader, auth: auth}
	request := httptest.NewRequest(http.MethodGet, "/v1/runs/"+testRunID+"/logs?attempt_id="+testAttemptID+"&limit=9&extra=x", nil)
	request.SetPathValue("id", testRunID)
	response := httptest.NewRecorder()
	api.getRunLogs(response, request)
	if response.Code != http.StatusBadRequest || reader.lookups != 0 || auth.calls != 0 {
		t.Fatalf("status=%d lookup=%d auth=%d", response.Code, reader.lookups, auth.calls)
	}
}

func TestRunLogsRejectsInconsistentStorePage(t *testing.T) {
	reader := &fakeRunLogReader{repository: "bsikar/ra8-firmware", page: store.LogPage{
		AttemptID: testAttemptID, NextAfter: 100, Chunks: []store.LogRecord{},
	}}
	api := &Server{logReader: reader, auth: &allowRunLogRead{}}
	request := httptest.NewRequest(http.MethodGet, "/v1/runs/"+testRunID+"/logs?attempt_id="+testAttemptID+"&after=0&limit=1", nil)
	request.SetPathValue("id", testRunID)
	response := httptest.NewRecorder()
	api.getRunLogs(response, request)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("inconsistent page status=%d", response.Code)
	}
}
