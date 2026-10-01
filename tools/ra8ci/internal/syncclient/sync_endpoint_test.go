// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"net/url"
	"strings"
	"testing"
)

func TestABareOriginNamesTheSyncEndpoint(t *testing.T) {
	endpoint, err := syncEndpoint("https://plane.example")
	if err != nil {
		t.Fatalf("a bare HTTPS origin must be accepted: %v", err)
	}
	if endpoint != "https://plane.example"+SyncPath {
		t.Fatalf("unexpected endpoint %q", endpoint)
	}
}

// The two raw shapes that parsed as a bare origin and concatenated into
// something else. Neither means anything about where to post, so neither
// decides it: the endpoint is the same one a bare origin names.
func TestEmptyQueryOrFragmentSyntaxDoesNotDecideTheEndpoint(t *testing.T) {
	want := "https://plane.example" + SyncPath
	for _, raw := range []string{"https://plane.example?", "https://plane.example#", "https://plane.example?#"} {
		parsed, err := url.Parse(raw)
		if err != nil {
			t.Fatalf("%q must parse for this test to mean anything: %v", raw, err)
		}
		if parsed.RawQuery != "" || parsed.Fragment != "" || parsed.Path != "" {
			t.Fatalf("this test is pointless if %q does not parse as a bare origin", raw)
		}
		endpoint, err := syncEndpoint(raw)
		if err != nil {
			t.Fatalf("%q parses as a bare origin and must be accepted: %v", raw, err)
		}
		if endpoint != want {
			t.Fatalf("%q produced %q, want %q", raw, endpoint, want)
		}
	}
}

// What the old concatenation produced, pinned so the reason this rule exists
// stays readable: the path this client names was not the path sent.
func TestTheOldConcatenationLostThePath(t *testing.T) {
	for _, raw := range []string{"https://plane.example?", "https://plane.example#"} {
		concatenated, err := url.Parse(strings.TrimSuffix(raw, "/") + SyncPath)
		if err != nil {
			t.Fatalf("parse %q: %v", raw, err)
		}
		if concatenated.Path != "" {
			t.Fatalf("expected %q to concatenate into an empty path, got %q", raw, concatenated.Path)
		}
	}
}

// The endpoint is built from the parsed URL, so what is sent parses back to
// the origin the checks were answered about, and to this path.
func TestTheEndpointParsesBackToTheCheckedOrigin(t *testing.T) {
	endpoint, err := syncEndpoint("https://plane.example:8443")
	if err != nil {
		t.Fatalf("an origin with a port must be accepted: %v", err)
	}
	parsed, err := url.Parse(endpoint)
	if err != nil {
		t.Fatalf("the endpoint must parse: %v", err)
	}
	if parsed.Scheme != "https" || parsed.Host != "plane.example:8443" ||
		parsed.Path != SyncPath || parsed.RawQuery != "" || parsed.Fragment != "" {
		t.Fatalf("endpoint %q did not keep the checked origin", endpoint)
	}
}

// A trailing "?" must not survive into the endpoint as an empty query either.
func TestTheEndpointCarriesNoTrailingQuestionMark(t *testing.T) {
	endpoint, err := syncEndpoint("https://plane.example?")
	if err != nil {
		t.Fatalf("unexpected refusal: %v", err)
	}
	if strings.HasSuffix(endpoint, "?") {
		t.Fatalf("endpoint %q kept an empty query", endpoint)
	}
}

// Every rule this package already applied, still applied, with the refusal
// wording unchanged so an operator reading the error sees the same sentence.
func TestAnythingOtherThanABareHTTPSOriginIsRefused(t *testing.T) {
	for _, tc := range []struct {
		name string
		raw  string
	}{
		{"plain HTTP", "http://plane.example"},
		{"no scheme", "plane.example"},
		{"no host", "https://"},
		{"credentials", "https://user:pass@plane.example"},
		{"a path", "https://plane.example/v1"},
		{"a trailing slash", "https://plane.example/"},
		{"a query", "https://plane.example?a=b"},
		{"a fragment", "https://plane.example#top"},
		{"opaque", "https:plane.example"},
		{"empty", ""},
	} {
		_, err := syncEndpoint(tc.raw)
		if err == nil {
			t.Fatalf("%s (%q) must be refused", tc.name, tc.raw)
		}
		if !strings.Contains(err.Error(), "must be an HTTPS origin") {
			t.Fatalf("%s: refusal wording changed: %v", tc.name, err)
		}
	}
}
