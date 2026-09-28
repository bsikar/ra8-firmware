// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// Two more doors that answer before the store is reached: the Terraform
// state backend, whose reservation ID is judged first, and the agent
// dispatch endpoint, which stays shut until an operator has named the
// commit it will trust. Both are served by a plane whose store is a zero
// value, so anything that reached the database would panic rather than
// pass.

// A reservation ID the plane could never have issued is refused before a
// lookup, which matters here more than on the run doors: Terraform sends
// this backend state bodies, and a backend that consulted the database on
// any path a caller wrote would be reachable by anyone who can spell a URL.
func TestARunnerReservationIsJudgedBeforeTheStoreIsAsked(t *testing.T) {
	handler := planeWithNoDatabase(t)
	for name, id := range map[string]string{
		"blank":            "%20",
		"not a UUID":       "reservation-17",
		"wrong version":    "00000000-0000-4000-8000-000000000001",
		"truncated":        "00000000-0000-7000-8000-00000000000",
		"trailing text":    "00000000-0000-7000-8000-000000000001x",
		"uppercase hex":    "00000000-0000-7000-8000-00000000000A",
		"path traversal":   "..%2F..%2Fetc",
		"embedded newline": "00000000-0000-7000-8000-000000000001%0Aevil",
	} {
		for _, method := range []string{http.MethodGet, http.MethodPost, http.MethodDelete} {
			t.Run(name+"/"+method, func(t *testing.T) {
				result := ask(t, handler, method, "/v1/terraform/runner-states/"+id)
				if result.status != http.StatusBadRequest {
					t.Fatalf("status = %d", result.status)
				}
				if result.body["code"] != "invalid_argument" || result.body["detail"] != "invalid runner reservation ID" {
					t.Fatalf("body = %+v", result.body)
				}
				if result.body["retryable"] != false {
					t.Fatalf("a malformed reservation was reported retryable: %+v", result.body)
				}
			})
		}
	}
}

// Terraform's own lock verbs are judged on the same identifier as the rest,
// so a caller cannot reach past the check by asking in a method the router
// was not written around.
func TestTheLockVerbsAreJudgedOnTheSameReservation(t *testing.T) {
	handler := planeWithNoDatabase(t)
	for _, method := range []string{"LOCK", "UNLOCK", http.MethodPut, http.MethodPatch, http.MethodHead} {
		result := ask(t, handler, method, "/v1/terraform/runner-states/reservation-17")
		if result.status != http.StatusBadRequest {
			t.Fatalf("%s status = %d", method, result.status)
		}
	}
}

// An unconfigured trusted commit keeps agent dispatch shut, and shut before
// the peer certificate is even looked at: a plane that has not been told
// which agent build it trusts has nothing safe to hand out, so it says so
// rather than claiming work for a build nobody reviewed. The refusal is
// retryable, because configuring the commit is what makes it answerable.
func TestAgentClaimStaysShutUntilATrustedCommitIsNamed(t *testing.T) {
	result := ask(t, planeWithNoDatabase(t), http.MethodPost, "/v1/agents/me/claim")
	if result.status != http.StatusServiceUnavailable {
		t.Fatalf("status = %d", result.status)
	}
	if result.body["code"] != "unavailable" || result.body["detail"] != "trusted agent commit is not configured" {
		t.Fatalf("body = %+v", result.body)
	}
	if result.body["retryable"] != true {
		t.Fatalf("a configuration gap was reported unretryable: %+v", result.body)
	}
	if result.thread == "" {
		t.Fatal("the refusal carried no correlation thread")
	}
}

// A door the plane does not serve is a 404 from the router, not a panic on
// a store it never had, and a run door asked in the wrong method is a 405.
// Both are answered without the database, and both come from net/http's own
// mux rather than the plane, so they carry plain text rather than a problem
// document: worth pinning, because a caller that parses every refusal as
// JSON would break on the two refusals it never reaches the plane for.
func TestUnroutedAndMismethodedDoorsAnswerWithoutTheStore(t *testing.T) {
	handler := planeWithNoDatabase(t)
	id := "00000000-0000-7000-8000-000000000001"
	for name, request := range map[string][3]any{
		"a path nothing serves":    {http.MethodGet, "/v1/nope", http.StatusNotFound},
		"a version nothing serves": {http.MethodGet, "/v2/runs/" + id, http.StatusNotFound},
		"reading the cancel door":  {http.MethodGet, "/v1/runs/" + id + "/cancel", http.StatusMethodNotAllowed},
		"posting to a run":         {http.MethodPost, "/v1/runs/" + id, http.StatusMethodNotAllowed},
		"posting to liveness":      {http.MethodPost, "/health/live", http.StatusMethodNotAllowed},
	} {
		t.Run(name, func(t *testing.T) {
			response := httptest.NewRecorder()
			handler.ServeHTTP(response, httptest.NewRequest(request[0].(string), request[1].(string), nil))
			if response.Code != request[2].(int) {
				t.Fatalf("status = %d, want %d", response.Code, request[2].(int))
			}
			if strings.Contains(response.Header().Get("Content-Type"), "json") {
				t.Fatalf("the router's own refusal claimed to be JSON: %q", response.Body.String())
			}
		})
	}
}
