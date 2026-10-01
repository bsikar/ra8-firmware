// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// What the run-log door says when something behind it will not answer.
//
// server_logs_test.go holds the happy path, the malformed query and the
// inconsistent page. What it does not hold is the three ways the door's own
// dependencies fail underneath it: no reader configured at all, the run
// lookup failing, and the page read failing after authorization has already
// passed. Each has to answer differently, because an operator reading the
// response is deciding whether to retry, to fix a request, or to go and look
// at the database.

// refusingLogReader fails on whichever half the test names, so a case can put
// a failure on exactly one side of the authorization check.
type refusingLogReader struct {
	repository string
	lookupErr  error
	pageErr    error
	page       store.LogPage
	pageCalls  int
}

func (f *refusingLogReader) LookupRunRepository(_ context.Context, _ string) (string, error) {
	if f.lookupErr != nil {
		return "", f.lookupErr
	}
	return f.repository, nil
}

func (f *refusingLogReader) AttemptLogs(_ context.Context, _, attemptID string, after int64, _ int) (store.LogPage, error) {
	f.pageCalls++
	if f.pageErr != nil {
		return store.LogPage{}, f.pageErr
	}
	page := f.page
	page.AttemptID, page.NextAfter = attemptID, after
	return page, nil
}

type refusingLogAuth struct{ err error }

func (a refusingLogAuth) Authorize(_ *http.Request, _, _ string) (string, error) {
	if a.err != nil {
		return "", a.err
	}
	return "reader", nil
}

type willingAuditor struct{ calls int }

func (a *willingAuditor) AuditDenied(context.Context, string, string, string) error {
	a.calls++
	return nil
}

func askForLogs(api *Server) *httptest.ResponseRecorder {
	request := httptest.NewRequest(http.MethodGet,
		"/v1/runs/"+testRunID+"/logs?attempt_id="+testAttemptID+"&after=0&limit=1", nil)
	request.SetPathValue("id", testRunID)
	response := httptest.NewRecorder()
	api.getRunLogs(response, request)
	return response
}

func TestRunLogsWithoutAReaderIsUnavailableNotEmpty(t *testing.T) {
	// An absent reader is a configuration fault, so the door must not answer
	// with an empty page: that would read as a run with no logs.
	response := askForLogs(&Server{auth: refusingLogAuth{}})
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), "run log reader is not configured") {
		t.Fatalf("the answer does not name the missing reader: %s", response.Body.String())
	}
	if !strings.Contains(response.Body.String(), `"retryable":true`) {
		t.Fatalf("an unconfigured reader should be reported retryable: %s", response.Body.String())
	}
}

func TestRunLogsCarryTheRunLookupsOwnRefusal(t *testing.T) {
	// The lookup runs before authorization, so an unknown run is a 404 from
	// the store rather than a denial, and a broken database is a 503.
	for _, that := range []struct {
		named string
		err   error
		want  int
	}{
		{"an unknown run", store.ErrNotFound, http.StatusNotFound},
		{"a database that will not answer", store.ErrUnavailable, http.StatusServiceUnavailable},
	} {
		t.Run(that.named, func(t *testing.T) {
			reader := &refusingLogReader{lookupErr: that.err}
			response := askForLogs(&Server{logReader: reader, auth: refusingLogAuth{}})
			if response.Code != that.want {
				t.Fatalf("status=%d want=%d body=%s", response.Code, that.want, response.Body.String())
			}
			if reader.pageCalls != 0 {
				t.Fatalf("the page was read after the lookup had already failed")
			}
		})
	}
}

func TestRunLogsDeniedReadIsAuditedAndNeverNamesTheRun(t *testing.T) {
	reader := &refusingLogReader{repository: "bsikar/ra8-firmware"}
	auditor := &willingAuditor{}
	api := &Server{logReader: reader, auth: refusingLogAuth{err: store.ErrDenied}, audit: auditor}

	response := askForLogs(api)
	if response.Code != http.StatusNotFound {
		t.Fatalf("a denied read answered %d: %s", response.Code, response.Body.String())
	}
	// A denial answers 404 rather than 403 so that asking is not itself a way
	// to learn which runs exist.
	if !strings.Contains(response.Body.String(), "run not found or access denied") {
		t.Fatalf("a denial should not distinguish itself from an absence: %s", response.Body.String())
	}
	if auditor.calls != 1 {
		t.Fatalf("the denial was not audited exactly once: %d", auditor.calls)
	}
	if reader.pageCalls != 0 {
		t.Fatalf("logs were read for a caller that was denied")
	}
}

func TestRunLogsDeniedWithoutAnAuditIsUnavailable(t *testing.T) {
	// Nothing records the denial here, and the door refuses rather than
	// answering on a decision it cannot write down.
	api := &Server{
		logReader: &refusingLogReader{repository: "bsikar/ra8-firmware"},
		auth:      refusingLogAuth{err: store.ErrDenied},
	}
	response := askForLogs(api)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), "authorization audit unavailable") {
		t.Fatalf("the answer does not name the missing audit: %s", response.Body.String())
	}
}

func TestRunLogsCarryThePageReadsOwnRefusal(t *testing.T) {
	// This failure happens after authorization has passed, so the caller is
	// entitled to the real reason rather than a denial.
	reader := &refusingLogReader{
		repository: "bsikar/ra8-firmware",
		pageErr:    errors.New("connection reset"),
	}
	response := askForLogs(&Server{logReader: reader, auth: refusingLogAuth{}, audit: &willingAuditor{}})
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), "database operation unavailable") {
		t.Fatalf("an unknown store failure should not be reported in its own words: %s", response.Body.String())
	}
	if reader.pageCalls != 1 {
		t.Fatalf("page reads=%d, want exactly one", reader.pageCalls)
	}
}
