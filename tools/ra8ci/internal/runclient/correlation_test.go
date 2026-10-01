// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/correlate"
)

func TestEveryRunRequestCarriesAThread(t *testing.T) {
	var seen []string
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seen = append(seen, r.Header.Get(correlate.Header))
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"id":"00000000-0000-7000-8000-000000000001","state":"queued"}`))
	}))
	defer server.Close()

	client := testClient(server)
	for i := 0; i < 2; i++ {
		if _, err := client.Get(context.Background(), "00000000-0000-7000-8000-000000000001"); err != nil {
			t.Fatalf("get %d: %v", i, err)
		}
	}
	if len(seen) != 2 {
		t.Fatalf("requests served: got %d, want 2", len(seen))
	}
	for i, id := range seen {
		if !correlate.Valid(id) {
			t.Fatalf("request %d carried an identifier the server would replace: %q", i, id)
		}
	}
	if seen[0] == seen[1] {
		t.Fatalf("two unpinned requests shared one thread: %q", seen[0])
	}
}

func TestOneJobCanPinOneThreadAcrossItsRequests(t *testing.T) {
	var seen []string
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seen = append(seen, r.Header.Get(correlate.Header))
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"id":"00000000-0000-7000-8000-000000000001","state":"queued"}`))
	}))
	defer server.Close()

	ctx, err := WithCorrelationID(context.Background(), "ci-job-4417.attempt-2")
	if err != nil {
		t.Fatalf("pinning a well-formed identifier: %v", err)
	}
	if got := CorrelationIDFrom(ctx); got != "ci-job-4417.attempt-2" {
		t.Fatalf("pinned identifier read back: got %q", got)
	}
	client := testClient(server)
	for i := 0; i < 3; i++ {
		if _, err := client.Get(ctx, "00000000-0000-7000-8000-000000000001"); err != nil {
			t.Fatalf("get %d: %v", i, err)
		}
	}
	for i, id := range seen {
		if id != "ci-job-4417.attempt-2" {
			t.Fatalf("request %d left the pinned thread: %q", i, id)
		}
	}
}

func TestAnIdentifierTheServerWouldReplaceIsRefusedBeforeItIsSent(t *testing.T) {
	for name, id := range map[string]string{
		"empty":      "",
		"space":      "ci job 4417",
		"newline":    "ci-4417\nX-Correlation-Id: someone-elses",
		"over bound": strings.Repeat("a", correlate.MaxID+1),
	} {
		t.Run(name, func(t *testing.T) {
			ctx, err := WithCorrelationID(context.Background(), id)
			if !errors.Is(err, ErrInvalidCorrelationID) {
				t.Fatalf("pinning %q: got %v, want ErrInvalidCorrelationID", id, err)
			}
			if got := CorrelationIDFrom(ctx); got != "" {
				t.Fatalf("a refused identifier was pinned anyway: %q", got)
			}
		})
	}
}

func TestAFailedRunRequestNamesTheThreadTheServerAnsweredWith(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set(correlate.Header, "server-chose-this")
		w.Header().Set("Content-Type", "application/problem+json")
		w.WriteHeader(http.StatusServiceUnavailable)
		_, _ = w.Write([]byte(`{"code":"unavailable","detail":"database operation unavailable","retryable":true,"correlation_id":"server-chose-this"}`))
	}))
	defer server.Close()

	ctx, err := WithCorrelationID(context.Background(), "sent-by-the-caller")
	if err != nil {
		t.Fatalf("pinning: %v", err)
	}
	_, err = testClient(server).Get(ctx, "00000000-0000-7000-8000-000000000001")
	if err == nil {
		t.Fatal("a 503 was not reported as a failure")
	}
	if !strings.Contains(err.Error(), "server-chose-this") || !strings.Contains(err.Error(), "503") {
		t.Fatalf("the operator cannot read the thread off the failure: %s", err.Error())
	}
	if strings.Contains(err.Error(), "sent-by-the-caller") {
		t.Fatalf("the failure names what was sent rather than what the server adopted: %s", err.Error())
	}
}

func TestAThreadIsReadFromTheProblemBodyWhenTheHeaderIsGone(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/problem+json")
		w.WriteHeader(http.StatusServiceUnavailable)
		_, _ = w.Write([]byte(`{"code":"unavailable","detail":"unavailable","retryable":true,"correlation_id":"only-in-the-body"}`))
	}))
	defer server.Close()

	_, err := testClient(server).Get(context.Background(), "00000000-0000-7000-8000-000000000001")
	if err == nil || !strings.Contains(err.Error(), "only-in-the-body") {
		t.Fatalf("body fallback: %v", err)
	}
}

func TestNoThreadIsInventedWhenTheServerNamedNone(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/problem+json")
		w.WriteHeader(http.StatusNotFound)
		_, _ = w.Write([]byte(`{"code":"not_found","detail":"run not found","retryable":false}`))
	}))
	defer server.Close()

	ctx, err := WithCorrelationID(context.Background(), "sent-by-the-caller")
	if err != nil {
		t.Fatalf("pinning: %v", err)
	}
	_, err = testClient(server).Get(ctx, "00000000-0000-7000-8000-000000000001")
	if err == nil {
		t.Fatal("a 404 was not reported as a failure")
	}
	if strings.Contains(err.Error(), "correlation") {
		t.Fatalf("the failure claims a thread it does not have: %s", err.Error())
	}
}
