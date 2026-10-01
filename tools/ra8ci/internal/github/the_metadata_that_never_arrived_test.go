// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

// Run metadata is what the plane records against a job: which attempt, which
// commit. A resolver that answered a zero JobMetadata with no error would
// have the plane file a job against attempt 0 and an empty commit, so every
// failure on the way to the forge has to arrive as a failure.

// resolvedAgainst builds a resolver whose requests all land on one handler,
// so the handler can refuse any single leg of the exchange. The existing
// harness in metadata_test.go serves a whole sound exchange inline and
// cannot refuse one leg on its own.
func resolvedAgainst(t *testing.T, handler http.HandlerFunc) *MetadataResolver {
	t.Helper()
	server := httptest.NewTLSServer(handler)
	t.Cleanup(server.Close)
	destination, err := url.Parse(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	client := server.Client()
	client.Transport = metadataTestTransport{destination: destination, transport: client.Transport}
	resolver, err := NewMetadataResolver(MetadataConfig{
		APIBaseURL: "https://api.github.com", AppClientID: "client-id", InstallationID: 42,
		PrivateKeyFile: soundKeyFile(t), Owner: "bsikar", Repository: "ra8-firmware",
		httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	return resolver
}

// mintedThen answers the installation token and hands everything else to
// the caller's handler.
func mintedThen(rest http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/app/installations/42/access_tokens" {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{
				Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour),
			})
			return
		}
		rest(w, r)
	}
}

// scaleSetJobToResolve is the job the resolver is asked about.
func scaleSetJobToResolve() Job {
	return Job{
		Owner: "bsikar", Repository: "ra8-firmware", JobID: "job-guid", WorkflowRunID: 1234,
		WorkflowRef: "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev",
		DisplayName: "build", EventName: "push",
	}
}

// A token the forge will not mint stops the resolution before the run is
// ever asked for. Asking anyway would send an unauthenticated read at the
// Actions API and read its refusal as a job that does not exist.
func TestATokenTheForgeWillNotMintStopsTheResolution(t *testing.T) {
	runAsks := 0
	resolver := resolvedAgainst(t, func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/repos/") {
			runAsks++
		}
		w.WriteHeader(http.StatusUnauthorized)
	})

	metadata, err := resolver.Resolve(context.Background(), scaleSetJobToResolve())
	if err == nil {
		t.Fatal("a refused token resolved metadata")
	}
	if metadata.WorkflowAttempt != 0 || metadata.CommitSHA != "" {
		t.Fatalf("a refused token answered %+v", metadata)
	}
	if runAsks != 0 {
		t.Fatalf("the Actions API was asked %d times without a token", runAsks)
	}
}

// A run fetch that drops is reported as the fetch it was. An empty read
// would otherwise look like a run that does not match, which is the same
// answer the resolver gives a genuinely foreign job.
func TestARunFetchThatDroppedIsAFailedFetch(t *testing.T) {
	resolver := resolvedAgainst(t, mintedThen(hangUp))

	metadata, err := resolver.Resolve(context.Background(), scaleSetJobToResolve())
	if err == nil || !strings.Contains(err.Error(), "fetch GitHub workflow metadata") {
		t.Fatalf("a dropped fetch answered %v", err)
	}
	if metadata.WorkflowAttempt != 0 || metadata.WorkflowRunID != 0 {
		t.Fatalf("a dropped fetch answered %+v", metadata)
	}
}

// A run document past the 1 MiB bound is refused before it is decoded, and
// one exactly at the bound is read. The bound is what keeps a flooding
// endpoint from spending the plane's memory on the way to a decode.
func TestARunDocumentPastTheBoundIsRefusedBeforeItIsDecoded(t *testing.T) {
	oversized := resolvedAgainst(t, mintedThen(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"padding":"`))
		_, _ = w.Write([]byte(strings.Repeat("p", maxMetadataResponse)))
		_, _ = w.Write([]byte(`"}`))
	}))
	_, err := oversized.Resolve(context.Background(), scaleSetJobToResolve())
	if err == nil || !strings.Contains(err.Error(), "unreadable or too large") {
		t.Fatalf("an oversized run document answered %v", err)
	}

	// A document at the bound is read, and then judged on its contents
	// rather than its size: this one is padding, so it does not match.
	padding := maxMetadataResponse - len(`{"padding":""}`)
	atBound := resolvedAgainst(t, mintedThen(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"padding":"` + strings.Repeat("p", padding) + `"}`))
	}))
	_, err = atBound.Resolve(context.Background(), scaleSetJobToResolve())
	if err == nil || !strings.Contains(err.Error(), "does not match the scale-set job") {
		t.Fatalf("a run document at the bound answered %v", err)
	}

	// A promised length the body never delivers is a failed read, not a
	// short run document.
	truncated := resolvedAgainst(t, mintedThen(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Content-Length", "4096")
		_, _ = w.Write([]byte(`{"id":1234,`))
		hangUp(w, r)
	}))
	_, err = truncated.Resolve(context.Background(), scaleSetJobToResolve())
	if err == nil {
		t.Fatal("a truncated run document resolved metadata")
	}
}

// Every status other than 200 is refused with the status named, so an
// operator can tell a revoked installation from a run that is simply gone.
func TestEveryUnhappyStatusOnTheRunFetchIsNamed(t *testing.T) {
	for _, status := range []int{
		http.StatusMovedPermanently, http.StatusUnauthorized, http.StatusForbidden,
		http.StatusNotFound, http.StatusTooManyRequests, http.StatusInternalServerError,
		http.StatusBadGateway, http.StatusServiceUnavailable,
	} {
		resolver := resolvedAgainst(t, mintedThen(func(w http.ResponseWriter, _ *http.Request) {
			w.Header().Set("Location", "https://api.github.com/elsewhere")
			w.WriteHeader(status)
		}))
		_, err := resolver.Resolve(context.Background(), scaleSetJobToResolve())
		if err == nil || !strings.Contains(err.Error(), "GitHub workflow metadata returned HTTP") {
			t.Fatalf("HTTP %d answered %v", status, err)
		}
	}
}
