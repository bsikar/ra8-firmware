// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"math/big"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// What the mutual-TLS authorizer refuses, it refuses before it asks the
// database anything. Every case below runs against a zero store, whose
// AuthorizeCertificate would fault if it were reached, so a case that
// answers rather than faulting is itself the proof the refusal came first.
// server_test.go already holds the unauthenticated request and the empty
// verified chain; this takes the rest, because a grant check that can be
// walked around is not a grant check.

// peerInWindow mints a client leaf with the validity window it is given, so
// a certificate can be aged or post-dated without waiting.
func peerInWindow(t *testing.T, serial int64, notBefore, notAfter time.Time) *x509.Certificate {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(serial),
		Subject:      pkix.Name{CommonName: "operator"},
		NotBefore:    notBefore,
		NotAfter:     notAfter,
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parse certificate: %v", err)
	}
	return leaf
}

func presented(peer []*x509.Certificate, chains [][]*x509.Certificate) *http.Request {
	request := httptest.NewRequest(http.MethodGet, "/v1/runs/"+boardTestProofID, nil)
	request.TLS = &tls.ConnectionState{PeerCertificates: peer, VerifiedChains: chains}
	return request
}

func TestTheAuthorizerRefusesBeforeItAsksTheDatabase(t *testing.T) {
	live := livePeer(t)
	other := peerInWindow(t, 21, time.Now().Add(-time.Hour), time.Now().Add(time.Hour))
	notYet := peerInWindow(t, 22, time.Now().Add(time.Hour), time.Now().Add(2*time.Hour))
	expired := peerInWindow(t, 23, time.Now().Add(-2*time.Hour), time.Now().Add(-time.Hour))
	unparsed := &x509.Certificate{
		NotBefore: time.Now().Add(-time.Hour),
		NotAfter:  time.Now().Add(time.Hour),
	}

	for name, refused := range map[string]struct {
		authorizer MTLSAuthorizer
		request    *http.Request
	}{
		"a plane with no store to ask": {
			authorizer: MTLSAuthorizer{},
			request:    presented([]*x509.Certificate{live}, [][]*x509.Certificate{{live}}),
		},
		"a peer that presented nothing": {
			authorizer: MTLSAuthorizer{Store: &store.Store{}},
			request:    presented(nil, [][]*x509.Certificate{{live}}),
		},
		"a connection that verified nothing": {
			authorizer: MTLSAuthorizer{Store: &store.Store{}},
			request:    presented([]*x509.Certificate{live}, nil),
		},
		"a chain whose leaf is missing": {
			authorizer: MTLSAuthorizer{Store: &store.Store{}},
			request:    presented([]*x509.Certificate{live}, [][]*x509.Certificate{{nil}}),
		},
		"a leaf that is not the one verified": {
			authorizer: MTLSAuthorizer{Store: &store.Store{}},
			request:    presented([]*x509.Certificate{live}, [][]*x509.Certificate{{other}}),
		},
		"a leaf with nothing to hash": {
			authorizer: MTLSAuthorizer{Store: &store.Store{}},
			request:    presented([]*x509.Certificate{unparsed}, [][]*x509.Certificate{{unparsed}}),
		},
		"a certificate that is not valid yet": {
			authorizer: MTLSAuthorizer{Store: &store.Store{}},
			request:    presented([]*x509.Certificate{notYet}, [][]*x509.Certificate{{notYet}}),
		},
		"a certificate that has expired": {
			authorizer: MTLSAuthorizer{Store: &store.Store{}},
			request:    presented([]*x509.Certificate{expired}, [][]*x509.Certificate{{expired}}),
		},
	} {
		actor, err := refused.authorizer.Authorize(refused.request, "bsikar/ra8-firmware", "read")
		if err == nil {
			t.Fatalf("%s was authorized", name)
		}
		if !errors.Is(err, store.ErrDenied) {
			t.Fatalf("%s was refused as %v, want a denial", name, err)
		}
		if actor != "" {
			t.Fatalf("%s was refused but named actor %q", name, actor)
		}
	}
}

// NewWithBoardVerifier is the constructor an operator reaches for when lease
// transitions are wanted. It holds the same dependency floor as the plain
// one: a plane with no store or no reviewed catalog is a configuration
// error, not a plane that runs with the verifier switched off.
func TestABoardVerifierPlaneStillNeedsItsDependencies(t *testing.T) {
	cat := reviewedCatalog(t)

	if _, err := NewWithBoardVerifier(nil, cat, nil); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("a plane with no store was built: %v", err)
	}
	if _, err := NewWithBoardVerifier(&store.Store{}, nil, nil); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("a plane with no catalog was built: %v", err)
	}

	api, err := NewWithBoardVerifier(&store.Store{}, cat, nil)
	if err != nil {
		t.Fatalf("a plane with its dependencies was refused: %v", err)
	}
	if api == nil || api.Handler() == nil {
		t.Fatal("the plane was built without a handler")
	}
}

// The three run doors judge the ID in their path before they look anything
// up, so a malformed ID is answered on its own terms rather than becoming a
// lookup for a run that could not exist.
func TestTheRunDoorsJudgeTheirIDBeforeTheyLookAnythingUp(t *testing.T) {
	api, err := New(&store.Store{}, reviewedCatalog(t))
	if err != nil {
		t.Fatal(err)
	}

	for name, asked := range map[string]struct {
		method string
		target string
	}{
		"reading a run":          {method: http.MethodGet, target: "/v1/runs/not-a-run"},
		"cancelling a run":       {method: http.MethodPost, target: "/v1/runs/not-a-run/cancel"},
		"reading a run's events": {method: http.MethodGet, target: "/v1/runs/not-a-run/events"},
		"reading a run with a UUID that is not an ID": {
			method: http.MethodGet, target: "/v1/runs/f47ac10b-58cc-4372-a567-0e02b2c3d479",
		},
	} {
		request := httptest.NewRequest(asked.method, asked.target, nil)
		response := httptest.NewRecorder()
		api.Handler().ServeHTTP(response, request)

		if response.Code != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400: %s", name, response.Code, response.Body.String())
		}
	}
}
