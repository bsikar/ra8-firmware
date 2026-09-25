// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// checkRunTestTransport sends every request to the test server whatever host
// the publisher composed, so the publisher keeps its production api.github.com
// origin rule while the test stays deterministic.
type checkRunTestTransport struct {
	inner *http.Transport
	host  string
}

func (t checkRunTestTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	request = request.Clone(request.Context())
	request.URL.Host = t.host
	return t.inner.RoundTrip(request)
}

type checkRunServer struct {
	mu          sync.Mutex
	tokenBodies []installationTokenRequest
	runBodies   []checkRunRequest
	authHeaders []string
	status      int
	response    checkRunResponse
	echo        bool
}

func (s *checkRunServer) requests() (int, int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.tokenBodies), len(s.runBodies)
}

func newCheckRunPublisher(t *testing.T, mode CheckRunMode) (*CheckRunPublisher, *checkRunServer) {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	keyPath := filepath.Join(t.TempDir(), "app.pem")
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	if err := os.WriteFile(keyPath, keyPEM, 0600); err != nil {
		t.Fatalf("write key: %v", err)
	}
	state := &checkRunServer{status: http.StatusCreated, echo: true}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		state.mu.Lock()
		defer state.mu.Unlock()
		switch {
		case strings.HasSuffix(r.URL.Path, "/access_tokens"):
			var body installationTokenRequest
			_ = json.NewDecoder(r.Body).Decode(&body)
			state.tokenBodies = append(state.tokenBodies, body)
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour)})
		case strings.HasSuffix(r.URL.Path, "/check-runs"):
			var body checkRunRequest
			_ = json.NewDecoder(r.Body).Decode(&body)
			state.runBodies = append(state.runBodies, body)
			state.authHeaders = append(state.authHeaders, r.Header.Get("Authorization"))
			w.WriteHeader(state.status)
			if state.status != http.StatusCreated {
				return
			}
			created := state.response
			if state.echo {
				created = checkRunResponse{ID: 4242, Name: body.Name, HeadSHA: body.HeadSHA, Status: body.Status, Conclusion: body.Conclusion}
			}
			_ = json.NewEncoder(w).Encode(created)
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(server.Close)
	transport := server.Client().Transport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // deterministic test server
	client := &http.Client{Transport: checkRunTestTransport{inner: transport, host: strings.TrimPrefix(server.URL, "https://")}}
	publisher, err := NewCheckRunPublisher(CheckRunPublisherConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", Mode: mode, httpClient: client,
	})
	if err != nil {
		t.Fatalf("new publisher: %v", err)
	}
	return publisher, state
}

const testHeadSHA = "0123456789abcdef0123456789abcdef01234567"

// The publisher must never hold the read-only Actions token the metadata
// resolver mints, and the resolver must never hold one that can write a check
// run. The two differ in exactly the permission set and in nothing else.
func TestTheCheckRunTokenIsScopedToChecksWrite(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeShadow)
	run, err := NewTaskCheckRun(ModeShadow, "build", testHeadSHA, "failed")
	if err != nil {
		t.Fatalf("build run: %v", err)
	}
	if _, err := publisher.Publish(context.Background(), run, "compared against Actions"); err != nil {
		t.Fatalf("publish: %v", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	if len(server.tokenBodies) != 1 {
		t.Fatalf("minted %d tokens, want 1", len(server.tokenBodies))
	}
	minted := server.tokenBodies[0]
	if got := minted.Permissions["checks"]; got != "write" {
		t.Fatalf("checks permission %q, want write", got)
	}
	if _, reads := minted.Permissions["actions"]; reads {
		t.Fatal("the check run token also carries Actions access")
	}
	if len(minted.Permissions) != 1 {
		t.Fatalf("token carries %d permissions, want 1", len(minted.Permissions))
	}
	if len(minted.Repositories) != 1 || minted.Repositories[0] != "ra8-firmware" {
		t.Fatalf("token repositories %v", minted.Repositories)
	}
}

// Moving ra8ci onto the merge gate is a deployment change, not an argument a
// caller passes. A shadow publisher refuses an authoritative run before any
// request is built, so nothing is posted and no token is minted.
func TestAShadowPublisherRefusesAnAuthoritativeRun(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeShadow)
	run, err := NewTaskCheckRun(ModeAuthoritative, "build", testHeadSHA, "failed")
	if err != nil {
		t.Fatalf("build run: %v", err)
	}
	id, err := publisher.Publish(context.Background(), run, "summary")
	if !errors.Is(err, ErrCheckRunModeNotPermitted) {
		t.Fatalf("error %v, want ErrCheckRunModeNotPermitted", err)
	}
	if id != 0 {
		t.Fatalf("refused publish returned id %d", id)
	}
	if tokens, runs := server.requests(); tokens != 0 || runs != 0 {
		t.Fatalf("refused publish spoke to GitHub: %d token, %d check run requests", tokens, runs)
	}
}

// An authoritative publisher is the strongest mode, so it may also publish the
// shadow runs a comparison is still being made from.
func TestAnAuthoritativePublisherStillPublishesShadowRuns(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeAuthoritative)
	for _, mode := range []CheckRunMode{ModeShadow, ModeAuthoritative} {
		run, err := NewTaskCheckRun(mode, "build", testHeadSHA, "succeeded")
		if err != nil {
			t.Fatalf("build %s run: %v", mode, err)
		}
		if _, err := publisher.Publish(context.Background(), run, "summary"); err != nil {
			t.Fatalf("publish %s run: %v", mode, err)
		}
	}
	if _, runs := server.requests(); runs != 2 {
		t.Fatalf("posted %d check runs, want 2", runs)
	}
}

// What reaches GitHub is what the shadow rule decided: a neutral conclusion
// with the observed one still readable in the output title.
func TestThePostedShadowRunIsNeutralAndStillComparable(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeShadow)
	run, err := NewTaskCheckRun(ModeShadow, "build", testHeadSHA, "timed_out")
	if err != nil {
		t.Fatalf("build run: %v", err)
	}
	if _, err := publisher.Publish(context.Background(), run, "ra8ci shadow"); err != nil {
		t.Fatalf("publish: %v", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	if len(server.runBodies) != 1 {
		t.Fatalf("posted %d check runs", len(server.runBodies))
	}
	posted := server.runBodies[0]
	if posted.Conclusion != "neutral" {
		t.Fatalf("posted conclusion %q, want neutral", posted.Conclusion)
	}
	if posted.Name != "ra8ci-shadow / build" {
		t.Fatalf("posted name %q", posted.Name)
	}
	if posted.Status != "completed" || posted.HeadSHA != testHeadSHA {
		t.Fatalf("posted status %q head %q", posted.Status, posted.HeadSHA)
	}
	if !strings.Contains(posted.Output.Title, "timed_out") {
		t.Fatalf("posted title %q does not carry the observed conclusion", posted.Output.Title)
	}
	if server.authHeaders[0] != "Bearer installation-token" {
		t.Fatalf("authorization header %q", server.authHeaders[0])
	}
}

// GitHub is the record of what was published. A response naming a different
// run than the one posted is not a success, because the comparison against
// Actions would then be made against the wrong run.
func TestAResponseThatNamesAnotherRunIsRefused(t *testing.T) {
	for _, testCase := range []struct {
		name     string
		response checkRunResponse
	}{
		{name: "different name", response: checkRunResponse{ID: 1, Name: "ra8ci / build", Conclusion: "neutral"}},
		{name: "different conclusion", response: checkRunResponse{ID: 1, Name: "ra8ci-shadow / build", Conclusion: "success"}},
		{name: "no identifier", response: checkRunResponse{ID: 0, Name: "ra8ci-shadow / build", Conclusion: "neutral"}},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			publisher, server := newCheckRunPublisher(t, ModeShadow)
			server.mu.Lock()
			server.echo, server.response = false, testCase.response
			server.mu.Unlock()
			run, err := NewTaskCheckRun(ModeShadow, "build", testHeadSHA, "succeeded")
			if err != nil {
				t.Fatalf("build run: %v", err)
			}
			id, err := publisher.Publish(context.Background(), run, "summary")
			if err == nil || id != 0 {
				t.Fatalf("publish returned %d, %v", id, err)
			}
		})
	}
}

// A refusal from GitHub is reported as one, with the status it answered.
func TestGitHubRefusalIsReported(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeShadow)
	server.mu.Lock()
	server.status = http.StatusForbidden
	server.mu.Unlock()
	run, err := NewTaskCheckRun(ModeShadow, "build", testHeadSHA, "succeeded")
	if err != nil {
		t.Fatalf("build run: %v", err)
	}
	if _, err := publisher.Publish(context.Background(), run, "summary"); !errors.Is(err, ErrCheckRunRejected) {
		t.Fatalf("error %v, want ErrCheckRunRejected", err)
	}
}

// A token is minted once and reused while it is valid, so a run of tasks does
// not mint one token per check run.
func TestTheInstallationTokenIsReusedAcrossRuns(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeShadow)
	for _, state := range []string{"succeeded", "failed", "cancelled"} {
		run, err := NewTaskCheckRun(ModeShadow, "build", testHeadSHA, state)
		if err != nil {
			t.Fatalf("build run: %v", err)
		}
		if _, err := publisher.Publish(context.Background(), run, "summary"); err != nil {
			t.Fatalf("publish %s: %v", state, err)
		}
	}
	tokens, runs := server.requests()
	if tokens != 1 || runs != 3 {
		t.Fatalf("minted %d tokens for %d check runs, want 1 and 3", tokens, runs)
	}
}

// Configuration is refused before the key is read or anything is posted.
func TestPublisherConfigurationIsRefusedBeforeUse(t *testing.T) {
	base := CheckRunPublisherConfig{AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: "/nonexistent", Owner: "bsikar", Repository: "ra8-firmware"}
	for _, testCase := range []struct {
		name   string
		mutate func(*CheckRunPublisherConfig)
	}{
		{name: "mode outside the set", mutate: func(c *CheckRunPublisherConfig) { c.Mode = CheckRunMode(9) }},
		{name: "http base url", mutate: func(c *CheckRunPublisherConfig) { c.APIBaseURL = "http://api.github.com" }},
		{name: "another origin", mutate: func(c *CheckRunPublisherConfig) { c.APIBaseURL = "https://github.example.com" }},
		{name: "base url with a path", mutate: func(c *CheckRunPublisherConfig) { c.APIBaseURL = "https://api.github.com/api/v3" }},
		{name: "no installation", mutate: func(c *CheckRunPublisherConfig) { c.InstallationID = 0 }},
		{name: "empty owner", mutate: func(c *CheckRunPublisherConfig) { c.Owner = "" }},
		{name: "owner with a slash", mutate: func(c *CheckRunPublisherConfig) { c.Owner = "bsikar/ra8" }},
		{name: "empty repository", mutate: func(c *CheckRunPublisherConfig) { c.Repository = "" }},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			config := base
			testCase.mutate(&config)
			if publisher, err := NewCheckRunPublisher(config); err == nil || publisher != nil {
				t.Fatalf("accepted %+v", config)
			}
		})
	}
}

// An incomplete run is refused without a request, so a half-built check run
// never reaches GitHub.
func TestAnIncompleteRunIsNotPosted(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeShadow)
	for _, run := range []TaskCheckRun{
		{},
		{Name: "ra8ci-shadow / build", HeadSHA: "short", Status: "completed", Conclusion: "neutral"},
		{Name: "ra8ci-shadow / build", HeadSHA: testHeadSHA, Status: "in_progress", Conclusion: "neutral"},
		{Name: "ra8ci-shadow / build", HeadSHA: testHeadSHA, Status: "completed"},
	} {
		if _, err := publisher.Publish(context.Background(), run, "summary"); err == nil {
			t.Fatalf("published %+v", run)
		}
	}
	if tokens, runs := server.requests(); tokens != 0 || runs != 0 {
		t.Fatalf("incomplete runs spoke to GitHub: %d token, %d check run requests", tokens, runs)
	}
}

// Every posted run carries this plane's own identifier for it, so a later
// reconciliation can tell a run this deployment posted from one that merely
// shares the name.
func TestThePostedRunCarriesItsExternalIdentifier(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeShadow)
	run, err := NewTaskCheckRun(ModeShadow, "build", testHeadSHA, "failed")
	if err != nil {
		t.Fatalf("build run: %v", err)
	}
	if _, err := publisher.Publish(context.Background(), run, "ra8ci shadow"); err != nil {
		t.Fatalf("publish: %v", err)
	}
	want, err := CheckRunExternalID(run)
	if err != nil {
		t.Fatalf("external id: %v", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	if len(server.runBodies) != 1 {
		t.Fatalf("posted %d check runs", len(server.runBodies))
	}
	if server.runBodies[0].ExternalID != want {
		t.Fatalf("posted external id %q, want %q", server.runBodies[0].ExternalID, want)
	}
}

// An echoed identifier naming another run is refused for the same reason a
// name is: the comparison against Actions would be made against the wrong
// run. An answer carrying none is silence about the field, not a different
// run, and the listing a reconciliation reads carries it independently.
func TestAnEchoedExternalIdentifierIsCheckedOnlyWhenItIsThere(t *testing.T) {
	run, err := NewTaskCheckRun(ModeShadow, "build", testHeadSHA, "succeeded")
	if err != nil {
		t.Fatalf("build run: %v", err)
	}
	for _, testCase := range []struct {
		name       string
		externalID string
		published  bool
	}{
		{name: "no identifier echoed", externalID: "", published: true},
		{name: "another identifier echoed", externalID: "ra8ci-1-00000000000000000000000000000000"},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			publisher, server := newCheckRunPublisher(t, ModeShadow)
			server.mu.Lock()
			server.echo = false
			server.response = checkRunResponse{
				ID: 11, Name: run.Name, HeadSHA: testHeadSHA, Status: "completed",
				Conclusion: run.Conclusion, ExternalID: testCase.externalID,
			}
			server.mu.Unlock()
			id, err := publisher.Publish(context.Background(), run, "summary")
			if testCase.published {
				if err != nil || id != 11 {
					t.Fatalf("publish returned %d, %v", id, err)
				}
				return
			}
			if err == nil || id != 0 {
				t.Fatalf("publish returned %d, %v", id, err)
			}
			if !errors.Is(err, ErrCheckRunRejected) {
				t.Fatalf("error %v, want a rejected check run", err)
			}
		})
	}
}
