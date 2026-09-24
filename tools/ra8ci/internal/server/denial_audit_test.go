// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"errors"
	"math/big"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type deniedRecord struct{ actor, action, target string }

type recordingAuditor struct {
	records []deniedRecord
	err     error
}

func (a *recordingAuditor) AuditDenied(_ context.Context, actor, action, target string) error {
	a.records = append(a.records, deniedRecord{actor: actor, action: action, target: target})
	return a.err
}

// peerCertificate mints a client certificate and returns it with the SHA-256
// of its DER, which is the identifier api_principals.cert_sha256 holds.
func peerCertificate(t *testing.T, commonName string) (*x509.Certificate, string) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(7),
		Subject:      pkix.Name{CommonName: commonName, Organization: []string{"secret-org"}},
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parse certificate: %v", err)
	}
	sum := sha256.Sum256(der)
	return leaf, hex.EncodeToString(sum[:])
}

func deniedRequest(t *testing.T, leaf *x509.Certificate) *http.Request {
	t.Helper()
	request := httptest.NewRequest(http.MethodPost, "/v1/runs?repository=bsikar/ra8-firmware", strings.NewReader(`{"secret":"do-not-audit-me"}`))
	request.Header.Set("Authorization", "Bearer do-not-audit-me-either")
	if leaf != nil {
		request.TLS = &tls.ConnectionState{
			PeerCertificates: []*x509.Certificate{leaf},
			VerifiedChains:   [][]*x509.Certificate{{leaf}},
		}
	}
	return request
}

func TestDenialAuditsTheCertificateFingerprintAndNothingElse(t *testing.T) {
	leaf, fingerprint := peerCertificate(t, "ra8ci-operator")
	auditor := &recordingAuditor{}
	api := &Server{audit: auditor}
	response := httptest.NewRecorder()

	api.deny(response, deniedRequest(t, leaf), "run.create", "bsikar/ra8-firmware", store.ErrDenied)

	if len(auditor.records) != 1 {
		t.Fatalf("denial wrote %d audit records, want 1", len(auditor.records))
	}
	record := auditor.records[0]
	if record.actor != "certificate-sha256:"+fingerprint {
		t.Fatalf("audited actor %q, want the certificate fingerprint", record.actor)
	}
	if record.action != "run.create" || record.target != "bsikar/ra8-firmware" {
		t.Fatalf("audited the wrong action/target: %+v", record)
	}
	// The denial is written before the request has been understood, so
	// anything carried into it beyond the fingerprint would be text the
	// caller chose. None of the request may appear in the record.
	for _, forbidden := range []string{"do-not-audit-me", "do-not-audit-me-either", "secret-org", "ra8ci-operator", "Bearer"} {
		if strings.Contains(record.actor+record.action+record.target, forbidden) {
			t.Fatalf("audit record carries %q: %+v", forbidden, record)
		}
	}
	if response.Code != http.StatusNotFound {
		t.Fatalf("denial answered %d, want 404", response.Code)
	}
	if body := response.Body.String(); strings.Contains(body, fingerprint) {
		t.Fatalf("denial response leaks the fingerprint to the caller: %s", body)
	}
}

func TestDenialRecordsAnUnverifiedPeerRatherThanDescribingIt(t *testing.T) {
	auditor := &recordingAuditor{}
	api := &Server{audit: auditor}

	api.deny(httptest.NewRecorder(), deniedRequest(t, nil), "run.create", "bsikar/ra8-firmware", store.ErrDenied)

	if len(auditor.records) != 1 || auditor.records[0].actor != "unverified-peer" {
		t.Fatalf("unexpected audit for a peer with no certificate: %+v", auditor.records)
	}
}

func TestDenialFailsClosedWhenTheAuditCannotBeWritten(t *testing.T) {
	leaf, _ := peerCertificate(t, "ra8ci-operator")
	auditor := &recordingAuditor{err: errors.New("audit unavailable")}
	api := &Server{audit: auditor}
	response := httptest.NewRecorder()

	api.deny(response, deniedRequest(t, leaf), "run.create", "bsikar/ra8-firmware", store.ErrDenied)

	// An unrecorded denial must not be answered as an ordinary refusal: the
	// 404 is what a caller probing for a run they may not read receives, and
	// handing it out with no audit trail makes the probe free.
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("unwritable audit answered %d, want 503", response.Code)
	}
}

func TestDenialWithNoAuditSinkRefusesRatherThanSkippingTheRecord(t *testing.T) {
	leaf, _ := peerCertificate(t, "ra8ci-operator")
	response := httptest.NewRecorder()

	(&Server{}).deny(response, deniedRequest(t, leaf), "run.create", "bsikar/ra8-firmware", store.ErrDenied)

	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("server with no audit sink answered %d, want 503", response.Code)
	}
}

func TestDenialPrefersTheConfiguredAuditorOverTheStore(t *testing.T) {
	auditor := &recordingAuditor{}
	// A nil *store.Store is a live method receiver, so a server that reached
	// for it here would panic rather than use the seam.
	api := &Server{audit: auditor, store: (*store.Store)(nil)}

	api.deny(httptest.NewRecorder(), deniedRequest(t, nil), "run.create", "bsikar/ra8-firmware", store.ErrDenied)

	if len(auditor.records) != 1 {
		t.Fatalf("configured auditor was not used: %+v", auditor.records)
	}
}

func TestCertificateActorIgnoresEverythingButTheLeaf(t *testing.T) {
	leaf, fingerprint := peerCertificate(t, "ra8ci-operator")
	if got := certificateActor(deniedRequest(t, leaf)); got != "certificate-sha256:"+fingerprint {
		t.Fatalf("certificateActor = %q", got)
	}
	if got := certificateActor(nil); got != "unverified-peer" {
		t.Fatalf("nil request actor = %q", got)
	}
	empty := deniedRequest(t, nil)
	empty.TLS = &tls.ConnectionState{PeerCertificates: []*x509.Certificate{{}}}
	if got := certificateActor(empty); got != "unverified-peer" {
		t.Fatalf("certificate with no bytes actor = %q", got)
	}
}
