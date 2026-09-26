// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/tls"
	"crypto/x509"
	"errors"
	"strings"
	"testing"
	"time"
)

// at returns a clock reading whatever the test points it at, the way a
// long-lived process reads the wall clock between one request and the next.
func at(instant *time.Time) func() time.Time {
	return func() time.Time { return *instant }
}

func TestClientIdentitySourcePresentsAUsableCertificate(t *testing.T) {
	clock := testNow
	present, err := ClientIdentitySource(issue(t, clientTemplate()), at(&clock))
	if err != nil {
		t.Fatalf("a usable client identity was refused: %v", err)
	}
	held, err := present(&tls.CertificateRequestInfo{})
	if err != nil {
		t.Fatalf("the handshake was refused: %v", err)
	}
	if held == nil || len(held.Certificate) == 0 {
		t.Fatal("no certificate was presented")
	}
	if held.Leaf == nil {
		t.Fatal("the leaf was not parsed for the TLS stack")
	}
}

// The whole point of the callback: the identity was usable when the process
// started and is not usable now, and the operator gets the local refusal
// rather than a handshake error or a denial that reads like a missing grant.
func TestExpiryBetweenRequestsIsRefusedLocally(t *testing.T) {
	clock := testNow
	present, err := ClientIdentitySource(issue(t, clientTemplate()), at(&clock))
	if err != nil {
		t.Fatalf("a usable client identity was refused: %v", err)
	}
	if _, err := present(&tls.CertificateRequestInfo{}); err != nil {
		t.Fatalf("the first handshake was refused: %v", err)
	}
	clock = testNow.Add(2 * time.Hour)
	_, err = present(&tls.CertificateRequestInfo{})
	if err == nil || !strings.Contains(err.Error(), "expired at") {
		t.Fatalf("expected an expiry refusal after the certificate ran out, got %v", err)
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("the refusal is not classifiable as a local identity problem: %v", err)
	}
	if !strings.Contains(err.Error(), "ra8ci-client") {
		t.Fatalf("the refusal does not name the subject: %v", err)
	}
}

// A certificate minted a few minutes ahead of this host's clock is presentable
// once the clock reaches it, without restarting the process.
func TestACertificateBecomesPresentableWhenItsWindowOpens(t *testing.T) {
	early := clientTemplate()
	early.NotBefore = testNow.Add(time.Hour)
	early.NotAfter = testNow.Add(48 * time.Hour)
	clock := testNow.Add(2 * time.Hour)
	present, err := ClientIdentitySource(issue(t, early), at(&clock))
	if err != nil {
		t.Fatalf("a certificate inside its window was refused: %v", err)
	}
	if _, err := present(&tls.CertificateRequestInfo{}); err != nil {
		t.Fatalf("the handshake was refused: %v", err)
	}
}

func TestAnUnusableIdentityIsRefusedBeforeAnySocketOpens(t *testing.T) {
	expired := clientTemplate()
	expired.NotBefore = testNow.Add(-48 * time.Hour)
	expired.NotAfter = testNow.Add(-time.Hour)
	clock := testNow
	present, err := ClientIdentitySource(issue(t, expired), at(&clock))
	if err == nil || !strings.Contains(err.Error(), "expired at") {
		t.Fatalf("expected construction to refuse an expired identity, got %v", err)
	}
	if present != nil {
		t.Fatal("a refused identity still handed back a callback")
	}
}

func TestAServerCertificateIsNotPresentedAsAClientIdentity(t *testing.T) {
	serverOnly := clientTemplate()
	serverOnly.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
	clock := testNow
	if _, err := ClientIdentitySource(issue(t, serverOnly), at(&clock)); err == nil ||
		!strings.Contains(err.Error(), "client authentication") {
		t.Fatalf("expected a key-usage refusal, got %v", err)
	}
}

func TestAKeyPairWithNoCertificateIsRefused(t *testing.T) {
	clock := testNow
	if _, err := ClientIdentitySource(tls.Certificate{}, at(&clock)); err == nil ||
		!strings.Contains(err.Error(), "carries no certificate") {
		t.Fatalf("expected an empty key pair to be refused, got %v", err)
	}
}

// A nil clock is the ordinary call, and it must read the wall clock rather
// than a zero time, which would refuse every certificate ever issued.
func TestANilClockReadsTheWallClock(t *testing.T) {
	now := time.Now()
	template := clientTemplate()
	template.NotBefore, template.NotAfter = now.Add(-time.Hour), now.Add(time.Hour)
	present, err := ClientIdentitySource(issue(t, template), nil)
	if err != nil {
		t.Fatalf("a usable client identity was refused: %v", err)
	}
	if _, err := present(&tls.CertificateRequestInfo{}); err != nil {
		t.Fatalf("the handshake was refused: %v", err)
	}
}

// The callback is read from several requests at once on a client that keeps
// connections open, so it must not mutate what it hands back.
func TestRepeatedHandshakesPresentTheSameCertificate(t *testing.T) {
	clock := testNow
	present, err := ClientIdentitySource(issue(t, clientTemplate()), at(&clock))
	if err != nil {
		t.Fatalf("a usable client identity was refused: %v", err)
	}
	first, err := present(&tls.CertificateRequestInfo{})
	if err != nil {
		t.Fatalf("the first handshake was refused: %v", err)
	}
	second, err := present(&tls.CertificateRequestInfo{})
	if err != nil {
		t.Fatalf("the second handshake was refused: %v", err)
	}
	if first != second {
		t.Fatal("two handshakes were handed different certificates")
	}
	if Fingerprint(first.Leaf) != Fingerprint(second.Leaf) {
		t.Fatal("two handshakes were handed different leaves")
	}
}
