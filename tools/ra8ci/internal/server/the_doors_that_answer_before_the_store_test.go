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
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// planeWithNoDatabase is a fully routed server whose store is never reachable.
// Every request below is refused on its own arguments before the store is
// consulted, which is exactly the property under test: a malformed run ID or
// an unanswerable query must cost the database nothing.
func planeWithNoDatabase(t *testing.T) http.Handler {
	t.Helper()
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	api, err := New(&store.Store{}, cat)
	if err != nil {
		t.Fatal(err)
	}
	return api.Handler()
}

// answered is the decoded problem document, with the thread the response
// carried in its header.
type answered struct {
	status int
	thread string
	body   map[string]any
}

func ask(t *testing.T, handler http.Handler, method, target string) answered {
	t.Helper()
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(method, target, nil))
	result := answered{status: response.Code, thread: response.Header().Get(correlationHeader)}
	if response.Body.Len() > 0 {
		if err := json.Unmarshal(response.Body.Bytes(), &result.body); err != nil {
			t.Fatalf("%s %s answered a body that is not JSON: %q", method, target, response.Body.String())
		}
	}
	return result
}

// TestLiveIsAnsweredWithoutTheDatabase pins the one door that must answer when
// the database cannot. Liveness says the process is running; readiness is the
// question that consults the store, and conflating them would have an
// orchestrator restart a plane that was merely waiting on its database.
func TestLiveIsAnsweredWithoutTheDatabase(t *testing.T) {
	result := ask(t, planeWithNoDatabase(t), http.MethodGet, "/health/live")
	if result.status != http.StatusOK {
		t.Fatalf("liveness status = %d", result.status)
	}
	if result.body["status"] != "alive" {
		t.Fatalf("liveness body = %+v", result.body)
	}
	if result.thread == "" {
		t.Fatal("liveness answered without a correlation thread")
	}
}

// TestARunIDIsJudgedBeforeTheStoreIsAsked pins that all three run doors refuse
// an identifier the plane could never have issued, with the same code and
// status, before any lookup. They share the judgement, so they are pinned
// together: a door that started asking the database first would be the one
// that diverged.
func TestARunIDIsJudgedBeforeTheStoreIsAsked(t *testing.T) {
	handler := planeWithNoDatabase(t)
	for name, id := range map[string]string{
		"blank":            "%20",
		"not a UUID":       "run-17",
		"wrong version":    "00000000-0000-4000-8000-000000000001",
		"truncated":        "00000000-0000-7000-8000-00000000000",
		"trailing text":    "00000000-0000-7000-8000-000000000001x",
		"uppercase hex":    "00000000-0000-7000-8000-00000000000A",
		"path traversal":   "..%2F..%2Fetc",
		"embedded newline": "00000000-0000-7000-8000-000000000001%0Aevil",
	} {
		for door, request := range map[string][2]string{
			"read":   {http.MethodGet, "/v1/runs/" + id},
			"cancel": {http.MethodPost, "/v1/runs/" + id + "/cancel"},
			"events": {http.MethodGet, "/v1/runs/" + id + "/events"},
		} {
			t.Run(name+"/"+door, func(t *testing.T) {
				result := ask(t, handler, request[0], request[1])
				if result.status != http.StatusBadRequest {
					t.Fatalf("status = %d for %s", result.status, request[1])
				}
				if result.body["code"] != "invalid_argument" || result.body["detail"] != "invalid run ID" {
					t.Fatalf("body = %+v", result.body)
				}
				if result.body["retryable"] != false {
					t.Fatalf("a malformed identifier was reported retryable: %+v", result.body)
				}
			})
		}
	}
}

// TestAnEventQueryIsJudgedBeforeTheStoreIsAsked pins the event page query the
// plane will not accept. An unknown parameter is refused rather than ignored:
// a caller that believes it bounded a page and silently got the default would
// walk a cursor it never asked for.
func TestAnEventQueryIsJudgedBeforeTheStoreIsAsked(t *testing.T) {
	handler := planeWithNoDatabase(t)
	id := "00000000-0000-7000-8000-000000000001"
	for name, query := range map[string]string{
		"unknown parameter":  "?cursor=4",
		"after repeated":     "?after=1&after=2",
		"after not a number": "?after=soon",
		"after negative":     "?after=-1",
		"after overflows":    "?after=99999999999999999999",
		"limit repeated":     "?limit=1&limit=2",
		"limit not a number": "?limit=many",
		"limit zero":         "?limit=0",
		"limit negative":     "?limit=-5",
		"limit over bound":   "?limit=51",
	} {
		t.Run(name, func(t *testing.T) {
			result := ask(t, handler, http.MethodGet, "/v1/runs/"+id+"/events"+query)
			if result.status != http.StatusBadRequest {
				t.Fatalf("status = %d", result.status)
			}
			if result.body["code"] != "invalid_argument" || result.body["detail"] != "invalid run event query" {
				t.Fatalf("body = %+v", result.body)
			}
		})
	}
}

// TestCancellationRefusesABodyBeforeTheStoreIsAsked pins the one argument
// cancellation has beyond its identifier. Cancellation carries no parameters,
// so a body is a caller believing it asked for something the plane would
// never have read.
func TestCancellationRefusesABodyBeforeTheStoreIsAsked(t *testing.T) {
	handler := planeWithNoDatabase(t)
	request := httptest.NewRequest(http.MethodPost,
		"/v1/runs/00000000-0000-7000-8000-000000000001/cancel", strings.NewReader(`{"reason":"mistake"}`))
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("status = %d", response.Code)
	}
	var body map[string]any
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body["detail"] != "run cancellation does not accept a request body" {
		t.Fatalf("body = %+v", body)
	}
}

// TestEveryRefusalCarriesTheThreadItWasAskedUnder pins the property that makes
// these refusals searchable: the correlation identifier is on the response
// header and inside the problem document, and a well-formed one supplied by
// the caller is the one adopted, so a CI job can find its own refused request
// in the plane's output.
func TestEveryRefusalCarriesTheThreadItWasAskedUnder(t *testing.T) {
	handler := planeWithNoDatabase(t)
	t.Run("supplied thread is adopted", func(t *testing.T) {
		request := httptest.NewRequest(http.MethodGet, "/v1/runs/run-17", nil)
		request.Header.Set(correlationHeader, "job-4821")
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if response.Header().Get(correlationHeader) != "job-4821" {
			t.Fatalf("header thread = %q", response.Header().Get(correlationHeader))
		}
		var body map[string]any
		if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
			t.Fatal(err)
		}
		if body["correlation_id"] != "job-4821" {
			t.Fatalf("problem document thread = %+v", body["correlation_id"])
		}
	})
	t.Run("an unusable thread is replaced not reflected", func(t *testing.T) {
		for _, supplied := range []string{"has a space", "has\na newline", "naïve", strings.Repeat("x", 200)} {
			request := httptest.NewRequest(http.MethodGet, "/v1/runs/run-17", nil)
			request.Header.Set(correlationHeader, supplied)
			response := httptest.NewRecorder()
			handler.ServeHTTP(response, request)
			served := response.Header().Get(correlationHeader)
			if served == "" || served == supplied {
				t.Fatalf("thread %q was reflected or dropped, served %q", supplied, served)
			}
		}
	})
	t.Run("a route that does not exist still carries one", func(t *testing.T) {
		// The mux answers an unrouted path itself, in plain text, so this
		// pins the header alone: the wrapper sits outside the mux precisely
		// so a request that reaches no handler is still searchable.
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/nothing-here", nil))
		if response.Code != http.StatusNotFound || response.Header().Get(correlationHeader) == "" {
			t.Fatalf("status = %d thread = %q", response.Code, response.Header().Get(correlationHeader))
		}
	})
}
