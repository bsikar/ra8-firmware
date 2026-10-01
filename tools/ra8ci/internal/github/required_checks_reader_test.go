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

type protectionServer struct {
	mu          sync.Mutex
	tokenBodies []installationTokenRequest
	methods     []string
	paths       []string
	authHeaders []string
	status      int
	body        string
}

func newRequiredCheckReader(t *testing.T) (*RequiredCheckReader, *protectionServer) {
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
	state := &protectionServer{status: http.StatusOK, body: `{"required_status_checks":{"checks":[]}}`}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		state.mu.Lock()
		defer state.mu.Unlock()
		state.methods = append(state.methods, r.Method)
		state.paths = append(state.paths, r.URL.Path)
		if strings.HasSuffix(r.URL.Path, "/access_tokens") {
			var body installationTokenRequest
			_ = json.NewDecoder(r.Body).Decode(&body)
			state.tokenBodies = append(state.tokenBodies, body)
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour)})
			return
		}
		state.authHeaders = append(state.authHeaders, r.Header.Get("Authorization"))
		w.WriteHeader(state.status)
		if state.status == http.StatusOK {
			_, _ = w.Write([]byte(state.body))
		}
	}))
	t.Cleanup(server.Close)
	transport := server.Client().Transport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // deterministic test server
	client := &http.Client{Transport: checkRunTestTransport{inner: transport, host: strings.TrimPrefix(server.URL, "https://")}}
	reader, err := NewRequiredCheckReader(RequiredCheckReaderConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatalf("new reader: %v", err)
	}
	return reader, state
}

func (s *protectionServer) serve(status int, body string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.status, s.body = status, body
}

// The gate is reported in the order GitHub listed it, from checks[] rather than
// the deprecated flat list.
func TestRequiredContextsReadsTheGateGitHubReports(t *testing.T) {
	reader, server := newRequiredCheckReader(t)
	server.serve(http.StatusOK, `{"required_status_checks":{"strict":true,
		"checks":[{"context":"ra8ci / build","app_id":1},{"context":"CodeQL","app_id":2},{"context":"ra8ci / lint","app_id":1}],
		"contexts":["ra8ci / lint","CodeQL","ra8ci / build"]}}`)
	contexts, err := reader.RequiredContexts(context.Background(), "main")
	if err != nil {
		t.Fatalf("read protection: %v", err)
	}
	want := []string{"ra8ci / build", "CodeQL", "ra8ci / lint"}
	if len(contexts) != len(want) {
		t.Fatalf("read %v, want %v", contexts, want)
	}
	for i := range want {
		if contexts[i] != want[i] {
			t.Fatalf("read %v, want %v (order is checks[], not contexts[])", contexts, want)
		}
	}
}

// Reading the gate and posting a check run are different powers. This token
// carries administration:read and nothing else, on one repository.
func TestTheProtectionTokenIsScopedToAdministrationRead(t *testing.T) {
	reader, server := newRequiredCheckReader(t)
	if _, err := reader.RequiredContexts(context.Background(), "main"); err != nil {
		t.Fatalf("read protection: %v", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	if len(server.tokenBodies) != 1 {
		t.Fatalf("minted %d tokens, want 1", len(server.tokenBodies))
	}
	minted := server.tokenBodies[0]
	if got := minted.Permissions["administration"]; got != "read" {
		t.Fatalf("administration permission %q, want read", got)
	}
	if len(minted.Permissions) != 1 {
		t.Fatalf("token carries %d permissions, want 1: %v", len(minted.Permissions), minted.Permissions)
	}
	if _, writes := minted.Permissions["checks"]; writes {
		t.Fatal("the protection token also carries checks access")
	}
	if len(minted.Repositories) != 1 || minted.Repositories[0] != "ra8-firmware" {
		t.Fatalf("token repositories %v", minted.Repositories)
	}
}

// Discovering the gate must not be able to change it.
func TestReadingTheGateOnlyReads(t *testing.T) {
	reader, server := newRequiredCheckReader(t)
	if _, err := reader.RequiredContexts(context.Background(), "main"); err != nil {
		t.Fatalf("read protection: %v", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	for i, method := range server.methods {
		if strings.HasSuffix(server.paths[i], "/access_tokens") {
			continue
		}
		if method != http.MethodGet {
			t.Fatalf("request %d to %s used %s", i, server.paths[i], method)
		}
	}
	protectionRequests := 0
	for i, path := range server.paths {
		if strings.HasSuffix(path, "/branches/main/protection") {
			protectionRequests++
			if server.methods[i] != http.MethodGet {
				t.Fatalf("protection read used %s", server.methods[i])
			}
		}
	}
	if protectionRequests != 1 {
		t.Fatalf("made %d protection requests, want 1: %v", protectionRequests, server.paths)
	}
}

// A protection GitHub would not return is not an empty gate. An unprotected
// branch, a missing branch and an installation that cannot see administration
// all answer the same way, and a plan built from "nothing is required" proposes
// tearing down a gate this reader never saw.
func TestUnreadableProtectionIsNotReportedAsAnEmptyGate(t *testing.T) {
	for _, status := range []int{http.StatusNotFound, http.StatusForbidden, http.StatusUnauthorized, http.StatusInternalServerError} {
		reader, server := newRequiredCheckReader(t)
		server.serve(status, "")
		contexts, err := reader.RequiredContexts(context.Background(), "main")
		if !errors.Is(err, ErrBranchProtectionUnreadable) {
			t.Fatalf("HTTP %d returned %v, want ErrBranchProtectionUnreadable", status, err)
		}
		if contexts != nil {
			t.Fatalf("HTTP %d returned contexts %v alongside the refusal", status, contexts)
		}
	}
}

// A protected branch that requires no status check is an answer, not a failure.
func TestAProtectedBranchRequiringNothingIsAnAnswer(t *testing.T) {
	for name, body := range map[string]string{
		"no required section": `{"required_pull_request_reviews":{"required_approving_review_count":1}}`,
		"empty checks":        `{"required_status_checks":{"strict":false,"checks":[],"contexts":[]}}`,
	} {
		t.Run(name, func(t *testing.T) {
			reader, server := newRequiredCheckReader(t)
			server.serve(http.StatusOK, body)
			contexts, err := reader.RequiredContexts(context.Background(), "main")
			if err != nil {
				t.Fatalf("read protection: %v", err)
			}
			if contexts == nil || len(contexts) != 0 {
				t.Fatalf("read %#v, want an empty set", contexts)
			}
		})
	}
}

// contexts[] is the deprecated spelling of the same requirement, and is read
// only when checks[] is absent.
func TestTheDeprecatedContextsListIsReadWhenChecksIsAbsent(t *testing.T) {
	reader, server := newRequiredCheckReader(t)
	server.serve(http.StatusOK, `{"required_status_checks":{"contexts":["ra8ci / build","CodeQL"]}}`)
	contexts, err := reader.RequiredContexts(context.Background(), "main")
	if err != nil {
		t.Fatalf("read protection: %v", err)
	}
	if len(contexts) != 2 || contexts[0] != "ra8ci / build" || contexts[1] != "CodeQL" {
		t.Fatalf("read %v", contexts)
	}
}

// A gate that describes itself two ways is not one to plan against, and
// choosing a side would plan against a requirement nobody stated.
func TestChecksAndContextsDisagreeingIsRefusedRatherThanResolved(t *testing.T) {
	for name, body := range map[string]string{
		"contexts names one checks does not": `{"required_status_checks":{"checks":[{"context":"ra8ci / build"}],"contexts":["ra8ci / build","CodeQL"]}}`,
		"different names":                    `{"required_status_checks":{"checks":[{"context":"ra8ci / build"}],"contexts":["ra8ci / lint"]}}`,
		"multiplicity differs":               `{"required_status_checks":{"checks":[{"context":"ra8ci / build"},{"context":"ra8ci / build"}],"contexts":["ra8ci / build","CodeQL"]}}`,
	} {
		t.Run(name, func(t *testing.T) {
			reader, server := newRequiredCheckReader(t)
			server.serve(http.StatusOK, body)
			contexts, err := reader.RequiredContexts(context.Background(), "main")
			if !errors.Is(err, ErrRequiredCheckSetDisagrees) {
				t.Fatalf("returned %v, want ErrRequiredCheckSetDisagrees", err)
			}
			if contexts != nil {
				t.Fatalf("returned contexts %v alongside the refusal", contexts)
			}
		})
	}
}

// The two listings agreeing in a different order is the same gate, not a
// disagreement.
func TestTheSameGateListedInADifferentOrderIsNotADisagreement(t *testing.T) {
	reader, server := newRequiredCheckReader(t)
	server.serve(http.StatusOK, `{"required_status_checks":{"checks":[{"context":"b"},{"context":"a"}],"contexts":["a","b"]}}`)
	if _, err := reader.RequiredContexts(context.Background(), "main"); err != nil {
		t.Fatalf("read protection: %v", err)
	}
}

// The reader reports the gate; PlanRequiredChecks judges it. A reader that
// tidied a padded or repeated context would hide the state the operator needs
// to see, and the planner already refuses both.
func TestTheGateIsReportedVerbatimForThePlannerToJudge(t *testing.T) {
	reader, server := newRequiredCheckReader(t)
	server.serve(http.StatusOK, `{"required_status_checks":{"checks":[{"context":"ra8ci / build"},{"context":"ra8ci / build"}]}}`)
	contexts, err := reader.RequiredContexts(context.Background(), "main")
	if err != nil {
		t.Fatalf("read protection: %v", err)
	}
	if len(contexts) != 2 {
		t.Fatalf("read %v, want the repeat reported", contexts)
	}
	if _, err := PlanRequiredChecks(ModeAuthoritative, []string{"build"}, contexts); !errors.Is(err, ErrRequiredCheckSetAmbiguous) {
		t.Fatalf("planner returned %v, want ErrRequiredCheckSetAmbiguous", err)
	}
}

// A branch name this reader cannot state plainly is refused before a token is
// minted, so a name whose request path would have to be reasoned about never
// reaches GitHub.
func TestABranchNameItWouldHaveToEscapeIsRefusedBeforeAnyRequest(t *testing.T) {
	for _, branch := range []string{"", "..", "main/..", "../../etc/passwd", "feature//x", "main ", " main",
		"main branch", "-main", "main/", "/main", "réf", strings.Repeat("a", 251)} {
		reader, server := newRequiredCheckReader(t)
		if _, err := reader.RequiredContexts(context.Background(), branch); err == nil {
			t.Fatalf("branch %q was accepted", branch)
		}
		server.mu.Lock()
		requests := len(server.paths)
		server.mu.Unlock()
		if requests != 0 {
			t.Fatalf("branch %q made %d requests before being refused", branch, requests)
		}
	}
}

func TestRequiredCheckReaderRefusesInvalidConfiguration(t *testing.T) {
	keyPath := filepath.Join(t.TempDir(), "app.pem")
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	if err := os.WriteFile(keyPath, pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)}), 0600); err != nil {
		t.Fatalf("write key: %v", err)
	}
	valid := RequiredCheckReaderConfig{AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware"}
	for name, mutate := range map[string]func(*RequiredCheckReaderConfig){
		"no client id":       func(c *RequiredCheckReaderConfig) { c.AppClientID = "" },
		"no installation":    func(c *RequiredCheckReaderConfig) { c.InstallationID = 0 },
		"no key file":        func(c *RequiredCheckReaderConfig) { c.PrivateKeyFile = "" },
		"missing key file":   func(c *RequiredCheckReaderConfig) { c.PrivateKeyFile = filepath.Join(t.TempDir(), "absent.pem") },
		"no owner":           func(c *RequiredCheckReaderConfig) { c.Owner = "" },
		"no repository":      func(c *RequiredCheckReaderConfig) { c.Repository = "" },
		"owner with a slash": func(c *RequiredCheckReaderConfig) { c.Owner = "bsikar/ra8" },
		"other api origin":   func(c *RequiredCheckReaderConfig) { c.APIBaseURL = "https://example.invalid" },
	} {
		t.Run(name, func(t *testing.T) {
			config := valid
			mutate(&config)
			if _, err := NewRequiredCheckReader(config); err == nil {
				t.Fatal("configuration was accepted")
			}
		})
	}
}
