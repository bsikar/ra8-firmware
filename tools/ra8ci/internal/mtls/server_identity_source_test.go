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

func TestServerIdentitySourcePresentsAUsableCertificate(t *testing.T) {
	clock := testNow
	present, err := ServerIdentitySource(issue(t, serverTemplate()), at(&clock))
	if err != nil {
		t.Fatalf("a usable server identity was refused: %v", err)
	}
	held, err := present(&tls.ClientHelloInfo{})
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

// The whole point of the callback: the listener has been up for weeks, its
// certificate ran out this morning, and the refusal names it locally instead
// of every client reporting the server as unreachable.
func TestServerExpiryBetweenConnectionsIsRefusedLocally(t *testing.T) {
	clock := testNow
	present, err := ServerIdentitySource(issue(t, serverTemplate()), at(&clock))
	if err != nil {
		t.Fatalf("a usable server identity was refused: %v", err)
	}
	if _, err := present(&tls.ClientHelloInfo{}); err != nil {
		t.Fatalf("the first handshake was refused: %v", err)
	}
	clock = testNow.Add(2 * time.Hour)
	_, err = present(&tls.ClientHelloInfo{})
	if err == nil || !strings.Contains(err.Error(), "expired at") {
		t.Fatalf("expected an expiry refusal after the certificate ran out, got %v", err)
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("the refusal is not classifiable as a local identity problem: %v", err)
	}
	if !strings.Contains(err.Error(), "ra8ci-server") {
		t.Fatalf("the refusal does not name the subject: %v", err)
	}
}

// A certificate minted a few minutes ahead of this host's clock is presentable
// once the clock reaches it, without restarting the service.
func TestAServerCertificateBecomesPresentableWhenItsWindowOpens(t *testing.T) {
	early := serverTemplate()
	early.NotBefore = testNow.Add(time.Hour)
	early.NotAfter = testNow.Add(48 * time.Hour)
	clock := testNow.Add(2 * time.Hour)
	present, err := ServerIdentitySource(issue(t, early), at(&clock))
	if err != nil {
		t.Fatalf("a certificate inside its window was refused: %v", err)
	}
	if _, err := present(&tls.ClientHelloInfo{}); err != nil {
		t.Fatalf("the handshake was refused: %v", err)
	}
}

func TestAnUnusableServerIdentityIsRefusedBeforeTheSocketOpens(t *testing.T) {
	expired := serverTemplate()
	expired.NotBefore = testNow.Add(-48 * time.Hour)
	expired.NotAfter = testNow.Add(-time.Hour)
	clock := testNow
	present, err := ServerIdentitySource(issue(t, expired), at(&clock))
	if err == nil || !strings.Contains(err.Error(), "expired at") {
		t.Fatalf("expected construction to refuse an expired identity, got %v", err)
	}
	if present != nil {
		t.Fatal("a refused identity still handed back a callback")
	}
}

func TestAClientCertificateIsNotServedFromTheListener(t *testing.T) {
	clientOnly := serverTemplate()
	clientOnly.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}
	clock := testNow
	if _, err := ServerIdentitySource(issue(t, clientOnly), at(&clock)); err == nil ||
		!strings.Contains(err.Error(), "server authentication") {
		t.Fatalf("expected a key-usage refusal, got %v", err)
	}
}

func TestAnAuthorityIsNotServedFromTheListener(t *testing.T) {
	authority := serverTemplate()
	authority.IsCA = true
	authority.KeyUsage = x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign
	clock := testNow
	if _, err := ServerIdentitySource(issue(t, authority), at(&clock)); err == nil ||
		!strings.Contains(err.Error(), "certificate authority") {
		t.Fatalf("expected an authority to be refused, got %v", err)
	}
}

func TestAServerKeyPairWithNoCertificateIsRefused(t *testing.T) {
	clock := testNow
	if _, err := ServerIdentitySource(tls.Certificate{}, at(&clock)); err == nil ||
		!strings.Contains(err.Error(), "carries no certificate") {
		t.Fatalf("expected an empty key pair to be refused, got %v", err)
	}
}

// A nil clock is the ordinary call, and it must read the wall clock rather
// than a zero time, which would refuse every certificate ever issued.
func TestANilClockReadsTheWallClockOnTheListener(t *testing.T) {
	now := time.Now()
	template := serverTemplate()
	template.NotBefore, template.NotAfter = now.Add(-time.Hour), now.Add(time.Hour)
	present, err := ServerIdentitySource(issue(t, template), nil)
	if err != nil {
		t.Fatalf("a usable server identity was refused: %v", err)
	}
	if _, err := present(&tls.ClientHelloInfo{}); err != nil {
		t.Fatalf("the handshake was refused: %v", err)
	}
}

// Connections arrive concurrently on a listener, so the callback must not
// mutate what it hands back.
func TestRepeatedListenerHandshakesPresentTheSameCertificate(t *testing.T) {
	clock := testNow
	present, err := ServerIdentitySource(issue(t, serverTemplate()), at(&clock))
	if err != nil {
		t.Fatalf("a usable server identity was refused: %v", err)
	}
	first, err := present(&tls.ClientHelloInfo{})
	if err != nil {
		t.Fatalf("the first handshake was refused: %v", err)
	}
	second, err := present(&tls.ClientHelloInfo{})
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
