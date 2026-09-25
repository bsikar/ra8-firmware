// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func correlationHandler(t *testing.T, inner http.HandlerFunc) http.Handler {
	t.Helper()
	return withCorrelation(inner)
}

func TestEveryResponseCarriesACorrelationID(t *testing.T) {
	handler := correlationHandler(t, func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "alive"})
	})
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/health/live", nil))

	id := response.Header().Get(correlationHeader)
	if !validCorrelationID(id) {
		t.Fatalf("minted correlation id %q is not well formed", id)
	}
	if len(id) != 2*correlationIDBytes {
		t.Fatalf("minted correlation id %q is %d characters, want %d", id, len(id), 2*correlationIDBytes)
	}
}

func TestAHandlerThatWritesNothingStillReturnsTheThread(t *testing.T) {
	handler := correlationHandler(t, func(http.ResponseWriter, *http.Request) {})
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/", nil))

	if response.Header().Get(correlationHeader) == "" {
		t.Fatal("a request that produced no body came back with no correlation id")
	}
}

func TestACallersOwnCorrelationIDIsEchoed(t *testing.T) {
	const supplied = "ra8ci-run-4471.attempt~2"
	handler := correlationHandler(t, func(w http.ResponseWriter, _ *http.Request) {
		problem(w, http.StatusConflict, "conflict", "run already cancelled", false)
	})
	request := httptest.NewRequest(http.MethodPost, "/v1/runs/r1/cancel", nil)
	request.Header.Set(correlationHeader, supplied)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)

	if got := response.Header().Get(correlationHeader); got != supplied {
		t.Fatalf("correlation header is %q, want the supplied %q", got, supplied)
	}
	var body map[string]any
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode problem document: %v", err)
	}
	if body["correlation_id"] != supplied {
		t.Fatalf("problem document reports correlation_id %v, want %q", body["correlation_id"], supplied)
	}
	if body["code"] != "conflict" || body["retryable"] != false {
		t.Fatalf("correlation id displaced the problem document: %v", body)
	}
}

func TestAMalformedCorrelationIDIsReplacedRatherThanReflected(t *testing.T) {
	for name, supplied := range map[string]string{
		"newline":      "abc\r\nX-Injected: yes",
		"space":        "two words",
		"control byte": "abc\x00def",
		"non ascii":    "identifiant-\u00e9",
		"empty":        "",
		"too long":     strings.Repeat("a", maxCorrelationID+1),
	} {
		t.Run(name, func(t *testing.T) {
			handler := correlationHandler(t, func(w http.ResponseWriter, _ *http.Request) {
				problem(w, http.StatusBadRequest, "invalid_argument", "bad request", false)
			})
			request := httptest.NewRequest(http.MethodPost, "/v1/runs", nil)
			request.Header[correlationHeader] = []string{supplied}
			response := httptest.NewRecorder()
			handler.ServeHTTP(response, request)

			got := response.Header().Get(correlationHeader)
			if supplied != "" && got == supplied {
				t.Fatalf("correlation header reflected the supplied %q", supplied)
			}
			if !validCorrelationID(got) {
				t.Fatalf("replacement correlation id %q is not well formed", got)
			}
		})
	}
}

func TestAProblemDocumentWithoutTheWrapperReportsNoThread(t *testing.T) {
	response := httptest.NewRecorder()
	problem(response, http.StatusServiceUnavailable, "unavailable", "database unavailable", true)

	var body map[string]any
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode problem document: %v", err)
	}
	if _, reported := body["correlation_id"]; reported {
		t.Fatalf("a mux served without the wrapper invented a correlation id: %v", body["correlation_id"])
	}
	if body["status"] != float64(http.StatusServiceUnavailable) || body["retryable"] != true {
		t.Fatalf("problem document lost its own fields: %v", body)
	}
}

func TestTheCorrelationIDIsNotTheSameOnTwoRequests(t *testing.T) {
	handler := correlationHandler(t, func(http.ResponseWriter, *http.Request) {})
	seen := make(map[string]bool, 64)
	for i := 0; i < 64; i++ {
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/health/live", nil))
		id := response.Header().Get(correlationHeader)
		if seen[id] {
			t.Fatalf("correlation id %q was minted twice in 64 requests", id)
		}
		seen[id] = true
	}
}

func TestValidCorrelationIDAcceptsUnreservedCharactersOnly(t *testing.T) {
	for _, accepted := range []string{"a", "0", "A-Z_a-z.0~9", strings.Repeat("a", maxCorrelationID)} {
		if !validCorrelationID(accepted) {
			t.Fatalf("correlation id %q should be accepted", accepted)
		}
	}
	for _, refused := range []string{"has space", "semi;colon", "slash/es", "quote\"", "brace{}", "plus+"} {
		if validCorrelationID(refused) {
			t.Fatalf("correlation id %q should be refused", refused)
		}
	}
}
