// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"context"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
)

// Every page this scan reads comes back through one GET. That GET is the
// only place the clock touches the network, so it is the only place a
// malformed path, a refusing endpoint or a dishonest body can reach the
// rest of the tool. What it refuses, it refuses before a request is spent.

// servedBy answers every request with the handler and hands back a client
// pointed at it over its own TLS origin, which is what the API requires.
func servedBy(t *testing.T, handler http.HandlerFunc) *actionsAPI {
	t.Helper()
	server := httptest.NewTLSServer(handler)
	t.Cleanup(server.Close)
	api, err := newActionsAPIAt(server.URL, "token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	return api
}

// A client that was never configured refuses rather than dereferencing its
// way to a panic on the first page of a scan.
func TestAGetRefusesAClientThatWasNeverConfigured(t *testing.T) {
	var target map[string]any
	for name, api := range map[string]*actionsAPI{
		"no client at all": nil,
		"nothing set":      {},
		"no origin":        {client: http.DefaultClient, token: "token"},
		"no transport":     {base: &url.URL{Scheme: "https", Host: "api.github.com"}, token: "token"},
	} {
		if err := api.get(context.Background(), "repos/o/r/actions/runs", nil, &target); err == nil {
			t.Fatalf("%s: answered without an error", name)
		}
	}
}

// The endpoint is joined onto the origin as a path, so anything that could
// re-point it, add a query of its own or split the request line is refused
// before a request exists.
func TestAGetRefusesAPathThatCouldRepointIt(t *testing.T) {
	api := servedBy(t, func(w http.ResponseWriter, _ *http.Request) {
		t.Error("a refused path reached the endpoint")
		w.WriteHeader(http.StatusOK)
	})

	var target map[string]any
	for _, endpoint := range []string{
		"repos/o/r?per_page=100", "repos/o/r#fragment", "repos\\o\\r",
		"repos/o/r\rHost: elsewhere", "repos/o/r\nHost: elsewhere",
		"repos//r/actions/runs", "repos/o/../../secrets", "repos/./r",
		"", "..",
	} {
		err := api.get(context.Background(), endpoint, nil, &target)
		if err == nil || !strings.Contains(err.Error(), "invalid GitHub API path") {
			t.Fatalf("%q = %v, want the path refused", endpoint, err)
		}
	}
}

// An endpoint that refuses is reported with the status it gave, so an
// operator reading the failure can tell a missing repository from a
// throttled token.
func TestAGetReportsTheStatusTheEndpointGave(t *testing.T) {
	for _, status := range []int{http.StatusUnauthorized, http.StatusForbidden,
		http.StatusNotFound, http.StatusTooManyRequests, http.StatusBadGateway,
		http.StatusMultipleChoices, http.StatusInternalServerError} {
		api := servedBy(t, func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(status)
		})
		var target map[string]any
		err := api.get(context.Background(), "repos/o/r/actions/runs", nil, &target)
		if err == nil || !strings.Contains(err.Error(), "GitHub API GET failed") {
			t.Fatalf("HTTP %d = %v, want the status reported", status, err)
		}
	}
}

// A body that stops early is a failure, not an empty page. Reporting it as
// a page would quietly shorten a scan.
func TestAGetRefusesABodyThatStopsEarly(t *testing.T) {
	api := servedBy(t, func(w http.ResponseWriter, _ *http.Request) {
		hijacker, ok := w.(http.Hijacker)
		if !ok {
			t.Skip("the test server does not support hijacking")
		}
		connection, buffered, err := hijacker.Hijack()
		if err != nil {
			t.Error(err)
			return
		}
		defer connection.Close()
		_, _ = buffered.WriteString("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 4096\r\n\r\n")
		_, _ = buffered.WriteString(`{"total_count":`)
		_ = buffered.Flush()
	})

	var target map[string]any
	err := api.get(context.Background(), "repos/o/r/actions/runs", nil, &target)
	if err == nil {
		t.Fatal("a truncated body was accepted as a page")
	}
	if len(target) != 0 {
		t.Fatalf("a truncated body reached the caller as a page: %v", target)
	}
}

// A body that is not the JSON it claims to be is refused with what was
// being decoded, rather than handed on as a zero-valued page.
func TestAGetRefusesABodyThatIsNotWhatItClaims(t *testing.T) {
	api := servedBy(t, func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte("<!doctype html><title>sign in</title>"))
	})

	var target map[string]any
	err := api.get(context.Background(), "repos/o/r/actions/runs", nil, &target)
	if err == nil || !strings.Contains(err.Error(), "decode GitHub API response") {
		t.Fatalf("answered %v, want the body refused", err)
	}
}

// The query the caller asked for is the query that is sent, and the
// endpoint is joined under the origin's own path rather than replacing it.
func TestAGetSendsTheQueryItWasGiven(t *testing.T) {
	var asked string
	api := servedBy(t, func(w http.ResponseWriter, r *http.Request) {
		asked = r.URL.RequestURI()
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"total_count":1}`))
	})

	var target struct {
		TotalCount int `json:"total_count"`
	}
	query := url.Values{"per_page": []string{"100"}, "page": []string{"2"}}
	if err := api.get(context.Background(), "repos/o/r/actions/runs", query, &target); err != nil {
		t.Fatal(err)
	}
	if asked != "/repos/o/r/actions/runs?page=2&per_page=100" {
		t.Fatalf("asked for %q", asked)
	}
	if target.TotalCount != 1 {
		t.Fatalf("decoded %d, want the page read", target.TotalCount)
	}
}

// A repository is two path segments and neither may be a relative one:
// "o/.." under an origin is a request for something else entirely.
func TestARepositoryIsTwoSegmentsAndNeitherIsRelative(t *testing.T) {
	for _, repo := range []string{
		"bsikar/ra8-firmware", "o/r", "a-b.c/d_e.f",
	} {
		if !validRepo(repo) {
			t.Fatalf("%q was refused, want it taken", repo)
		}
	}
	for _, repo := range []string{
		"", "bsikar", "bsikar/ra8-firmware/extra", "/ra8-firmware", "bsikar/",
		"../ra8-firmware", "bsikar/..", "./r", "o/.", "bsikar/ra8 firmware",
		"bsikar/ra8-firmware?", "https://github.com/bsikar/ra8-firmware",
	} {
		if validRepo(repo) {
			t.Fatalf("%q was taken, want it refused", repo)
		}
	}
}
