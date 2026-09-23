package server

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestNewRequiresAllDependencies(t *testing.T) {
	if _, err := New(nil, nil); err == nil {
		t.Fatal("server accepted missing store and catalog")
	}
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := New(nil, cat); err == nil {
		t.Fatal("server accepted missing store")
	}
}

func TestAgentClaimRequiresReviewedCommit(t *testing.T) {
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	api, err := New(&store.Store{}, cat)
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/v1/agents/me/claim", nil)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("untrusted remote dispatch status=%d", response.Code)
	}
	if _, err := NewWithOptions(&store.Store{}, cat, nil, "not-a-commit"); err == nil {
		t.Fatal("invalid trusted commit accepted")
	}
}

func TestAgentCertificateExpiryIsRecheckedPerRequest(t *testing.T) {
	leaf := &x509.Certificate{Raw: []byte("expired"),
		NotBefore: time.Now().Add(-2 * time.Hour), NotAfter: time.Now().Add(-time.Hour)}
	request := httptest.NewRequest(http.MethodPost, "/v1/agents/me/claim", nil)
	request.TLS = &tls.ConnectionState{PeerCertificates: []*x509.Certificate{leaf},
		VerifiedChains: [][]*x509.Certificate{{leaf}}}
	if _, err := verifiedAgentCertificate(request); err == nil {
		t.Fatal("expired certificate reused on long-lived connection")
	}
}

func TestMTLSAuthorizerRejectsNoVerifiedPeer(t *testing.T) {
	request := httptest.NewRequest("GET", "/v1/runs", nil)
	if _, err := (MTLSAuthorizer{Store: &store.Store{}}).Authorize(request, "bsikar/ra8-firmware", "read"); err == nil {
		t.Fatal("unauthenticated HTTP request was authorized")
	}
}

func TestMTLSAuthorizerRejectsEmptyVerifiedChain(t *testing.T) {
	request := httptest.NewRequest("GET", "/v1/runs", nil)
	request.TLS = &tls.ConnectionState{VerifiedChains: [][]*x509.Certificate{{}}}
	if _, err := (MTLSAuthorizer{Store: &store.Store{}}).Authorize(request, "bsikar/ra8-firmware", "read"); err == nil {
		t.Fatal("empty verified chain was authorized")
	}
}

func TestReadinessRunsEveryConfiguredDependencyAndFailsClosed(t *testing.T) {
	failure := errors.New("backup attestation expired")
	calls := 0
	api := &Server{readinessChecks: []func(context.Context) error{
		func(context.Context) error { calls++; return nil },
		func(context.Context) error { calls++; return failure },
		func(context.Context) error { calls++; return nil },
	}}
	if err := api.checkReadiness(context.Background()); !errors.Is(err, failure) {
		t.Fatalf("readiness error = %v, want backup failure", err)
	}
	if calls != 2 {
		t.Fatalf("ran %d readiness checks, want 2", calls)
	}
}
