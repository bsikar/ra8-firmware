// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"crypto/tls"
	"crypto/x509"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// presentedRequest is deniedRequest's counterpart for the case that matters
// here: a peer that presented a certificate the listener did not verify.
func presentedRequest(t *testing.T, presented *x509.Certificate, verified *x509.Certificate) *http.Request {
	t.Helper()
	request := httptest.NewRequest(http.MethodPost, "/v1/runs?repository=bsikar/ra8-firmware", strings.NewReader(`{}`))
	state := &tls.ConnectionState{PeerCertificates: []*x509.Certificate{presented}}
	if verified != nil {
		state.VerifiedChains = [][]*x509.Certificate{{verified}}
	}
	request.TLS = state
	return request
}

// An actor string is read as an identity: an operator matches it against
// api_principals.cert_sha256. A fingerprint written from a leaf no chain
// verified would sit in the audit trail looking exactly like one written
// after the authorization path checked it.
func TestAnUnverifiedLeafIsNotAuditedAsACertificateIdentity(t *testing.T) {
	presented, fingerprint := peerCertificate(t, "ra8ci-operator")
	got := certificateActor(presentedRequest(t, presented, nil))
	if got != "unverified-peer" {
		t.Fatalf("actor for an unverified leaf = %q", got)
	}
	if strings.Contains(got, fingerprint) {
		t.Fatalf("the audit trail carries an unverified fingerprint: %q", got)
	}
}

// The presented leaf and the verified leaf must be the same certificate, the
// rule MTLSAuthorizer, verifiedAgentCertificate and AuthorizeBoardPeer all
// already apply.
func TestALeafOtherThanTheVerifiedOneIsNotAudited(t *testing.T) {
	presented, presentedFingerprint := peerCertificate(t, "ra8ci-operator")
	verified, _ := peerCertificate(t, "ra8ci-agent")
	got := certificateActor(presentedRequest(t, presented, verified))
	if got != "unverified-peer" {
		t.Fatalf("actor for a mismatched chain = %q", got)
	}
	if strings.Contains(got, presentedFingerprint) {
		t.Fatalf("the audit trail carries the presented rather than the verified leaf: %q", got)
	}
}

func TestAVerifiedLeafIsStillAuditedByFingerprint(t *testing.T) {
	leaf, fingerprint := peerCertificate(t, "ra8ci-operator")
	if got := certificateActor(presentedRequest(t, leaf, leaf)); got != "certificate-sha256:"+fingerprint {
		t.Fatalf("actor for a verified leaf = %q", got)
	}
}

// The denial path reads the same rule, so an unverified peer is recorded as
// unverified rather than described.
func TestDenialRecordsAnUnverifiedLeafAsUnverified(t *testing.T) {
	presented, fingerprint := peerCertificate(t, "ra8ci-operator")
	auditor := &recordingAuditor{}
	api := &Server{audit: auditor}

	api.deny(httptest.NewRecorder(), presentedRequest(t, presented, nil), "run.create", "bsikar/ra8-firmware", store.ErrDenied)

	if len(auditor.records) != 1 {
		t.Fatalf("denial wrote %d audit records, want 1", len(auditor.records))
	}
	if auditor.records[0].actor != "unverified-peer" {
		t.Fatalf("unexpected audited actor: %q", auditor.records[0].actor)
	}
	if strings.Contains(auditor.records[0].actor, fingerprint) {
		t.Fatalf("the audit record carries an unverified fingerprint: %q", auditor.records[0].actor)
	}
}

// An empty verified chain entry is the same case as no chain at all, and must
// not panic on the way to refusing it.
func TestAnEmptyVerifiedChainIsRefusedWithoutPanicking(t *testing.T) {
	presented, _ := peerCertificate(t, "ra8ci-operator")
	request := presentedRequest(t, presented, nil)
	request.TLS.VerifiedChains = [][]*x509.Certificate{{}}
	if got := certificateActor(request); got != "unverified-peer" {
		t.Fatalf("actor for an empty verified chain = %q", got)
	}
	request.TLS.VerifiedChains = [][]*x509.Certificate{{nil}}
	if got := certificateActor(request); got != "unverified-peer" {
		t.Fatalf("actor for a nil verified leaf = %q", got)
	}
}
