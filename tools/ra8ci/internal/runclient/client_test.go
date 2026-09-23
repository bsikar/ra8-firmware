// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func testClient(server *httptest.Server) *Client {
	base, _ := url.Parse(server.URL)
	return &Client{base: base, http: server.Client()}
}

func TestSubmitSendsIdempotencyAndStrictPayload(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/v1/runs" {
			t.Fatalf("request = %s %s", r.Method, r.URL.Path)
		}
		if r.Header.Get("Idempotency-Key") != "retry-7" || r.Header.Get("Content-Type") != "application/json" {
			t.Fatal("idempotency or content-type header missing")
		}
		var input SubmitRequest
		if err := json.NewDecoder(r.Body).Decode(&input); err != nil {
			t.Fatal(err)
		}
		if input.Trigger != "cli" || input.Source.Repository != "bsikar/ra8-firmware" ||
			input.Source.CommitSHA == "" || input.CatalogDigest == "" || len(input.Tasks) != 1 || input.Tasks[0].Name != "test-go" {
			t.Fatalf("unexpected payload: %+v", input)
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusCreated)
		_, _ = w.Write([]byte(`{"id":"00000000-0000-7000-8000-000000000001","state":"queued"}`))
	}))
	defer server.Close()

	client := testClient(server)
	receipt, err := client.Submit(context.Background(), "retry-7", SubmitRequest{
		Trigger: "cli", Source: Source{Repository: "bsikar/ra8-firmware", CommitSHA: strings.Repeat("a", 40)},
		CatalogDigest: strings.Repeat("b", 64), Tasks: []Task{{Key: "task-001", Name: "test-go", Args: []string{}}},
	})
	if err != nil {
		t.Fatal(err)
	}
	if receipt.State != "queued" || receipt.ID != "00000000-0000-7000-8000-000000000001" {
		t.Fatalf("unexpected receipt: %+v", receipt)
	}
}

func TestSubmitRejectsInvalidKeyAndReceipt(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"id":"bad","state":"queued"}`))
	}))
	defer server.Close()
	client := testClient(server)
	input := SubmitRequest{Tasks: []Task{{Key: "k", Name: "test-go"}}}
	if _, err := client.Submit(context.Background(), "bad\nkey", input); err == nil {
		t.Fatal("invalid idempotency key accepted")
	}
	if _, err := client.Submit(context.Background(), "valid-key", input); err == nil {
		t.Fatal("invalid server receipt accepted")
	}
}

func TestGetValidatesRequestedRunIdentity(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet || r.URL.Path != "/v1/runs/00000000-0000-7000-8000-000000000001" {
			t.Fatalf("request = %s %s", r.Method, r.URL.Path)
		}
		_, _ = w.Write([]byte(`{"id":"00000000-0000-7000-8000-000000000002","state":"queued"}`))
	}))
	defer server.Close()
	client := testClient(server)
	if _, err := client.Get(context.Background(), "00000000-0000-7000-8000-000000000001"); err == nil {
		t.Fatal("foreign run response accepted")
	}
}

func TestLogsValidatesPageCursorAndDigests(t *testing.T) {
	data := []byte("log line\n")
	sum := sha256.Sum256(data)
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet || r.URL.Path != "/v1/runs/00000000-0000-7000-8000-000000000001/logs" ||
			r.URL.Query().Get("attempt_id") != "00000000-0000-7000-8000-000000000002" ||
			r.URL.Query().Get("after") != "4" || r.URL.Query().Get("limit") != "1" {
			t.Fatalf("request = %s %s?%s", r.Method, r.URL.Path, r.URL.RawQuery)
		}
		_ = json.NewEncoder(w).Encode(store.LogPage{AttemptID: "00000000-0000-7000-8000-000000000002",
			Chunks: []store.LogRecord{{Sequence: 5, Stream: "stdout", SHA256: hex.EncodeToString(sum[:]),
				DataBase64: base64.StdEncoding.EncodeToString(data)}}, NextAfter: 5})
	}))
	defer server.Close()
	client := testClient(server)
	page, err := client.Logs(context.Background(), "00000000-0000-7000-8000-000000000001",
		"00000000-0000-7000-8000-000000000002", 4, 1)
	if err != nil {
		t.Fatal(err)
	}
	if page.NextAfter != 5 || string(page.Chunks[0].Data) != string(data) {
		t.Fatalf("unexpected page: %+v", page)
	}
}

func TestLogsRejectsDigestMismatch(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode(store.LogPage{AttemptID: "00000000-0000-7000-8000-000000000002",
			Chunks: []store.LogRecord{{Sequence: 1, Stream: "stderr", SHA256: strings.Repeat("0", 64),
				DataBase64: base64.StdEncoding.EncodeToString([]byte("bad"))}}, NextAfter: 1})
	}))
	defer server.Close()
	client := testClient(server)
	if _, err := client.Logs(context.Background(), "00000000-0000-7000-8000-000000000001",
		"00000000-0000-7000-8000-000000000002", 0, 1); err == nil {
		t.Fatal("tampered log digest accepted")
	}
}

func TestCancelSendsBodylessRequestAndValidatesRun(t *testing.T) {
	const runID = "00000000-0000-7000-8000-000000000001"
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/v1/runs/"+runID+"/cancel" || r.ContentLength != 0 {
			t.Fatalf("request = %s %s, length=%d", r.Method, r.URL.Path, r.ContentLength)
		}
		_, _ = w.Write([]byte(`{"id":"00000000-0000-7000-8000-000000000001","state":"running","cancel_requested_at":"2026-09-22T00:00:00Z","cancel_requested_by":"test"}`))
	}))
	defer server.Close()
	client := testClient(server)
	run, err := client.Cancel(context.Background(), runID)
	if err != nil || run.ID != runID || run.CancelRequestedAt == nil || run.CancelRequestedBy != "test" {
		t.Fatalf("Cancel() = %+v, %v", run, err)
	}
	if _, err := client.Cancel(context.Background(), "invalid"); err == nil {
		t.Fatal("invalid run ID accepted")
	}
}

func TestCancelRejectsForeignResponse(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"id":"00000000-0000-7000-8000-000000000002","state":"terminal"}`))
	}))
	defer server.Close()
	if _, err := testClient(server).Cancel(context.Background(), "00000000-0000-7000-8000-000000000001"); err == nil {
		t.Fatal("foreign cancellation response accepted")
	}
}
