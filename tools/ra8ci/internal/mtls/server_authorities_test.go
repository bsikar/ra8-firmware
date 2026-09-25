// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"errors"
	"math/big"
	"strings"
	"testing"
	"time"
)

// serverAuthorityPEM issues a self-signed authority and returns its PEM block
// along with the parsed certificate, so a test can state what it put in the
// bundle without re-reading it.
func serverAuthorityPEM(t *testing.T, template *x509.Certificate) ([]byte, *x509.Certificate) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	return pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), parsed
}

func serverAuthorityTemplate(name string, notBefore, notAfter time.Time) *x509.Certificate {
	return &x509.Certificate{
		SerialNumber:          big.NewInt(time.Now().UnixNano()),
		Subject:               pkix.Name{CommonName: name},
		NotBefore:             notBefore,
		NotAfter:              notAfter,
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageCRLSign,
	}
}

func TestServerAuthoritiesAcceptsALiveAuthority(t *testing.T) {
	now := time.Now()
	bundle, authority := serverAuthorityPEM(t, serverAuthorityTemplate("ra8ci-server-ca", now.Add(-time.Hour), now.Add(time.Hour)))
	pool, err := ServerAuthorities(bundle, now)
	if err != nil {
		t.Fatalf("a live server authority was refused: %v", err)
	}
	if pool == nil {
		t.Fatal("no pool returned for a usable bundle")
	}
	if !pool.Equal(poolOf(authority)) {
		t.Fatal("the returned pool does not hold the authority the bundle stated")
	}
}

func poolOf(authorities ...*x509.Certificate) *x509.CertPool {
	pool := x509.NewCertPool()
	for _, authority := range authorities {
		pool.AddCert(authority)
	}
	return pool
}

func TestServerAuthoritiesRefusesAnEmptyBundle(t *testing.T) {
	_, err := ServerAuthorities(nil, time.Now())
	if !errors.Is(err, ErrIdentity) || !strings.Contains(err.Error(), "server certificate authority bundle is empty") {
		t.Fatalf("an empty server CA bundle was not refused by name: %v", err)
	}
}

// A key file handed over as the trust bundle is the mistake the CLI makes
// easiest, since RA8CI_SERVER_CA and RA8CI_CLIENT_KEY sit beside each other.
func TestServerAuthoritiesRefusesABundleHoldingNoCertificate(t *testing.T) {
	notACertificate := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: []byte("not a certificate")})
	_, err := ServerAuthorities(notACertificate, time.Now())
	if !errors.Is(err, ErrIdentity) || !strings.Contains(err.Error(), "holds no certificate") {
		t.Fatalf("a bundle with no certificate was not refused: %v", err)
	}
}

// The server's own certificate is what an operator reaches for first when
// asked for the CA that issued it, and AppendCertsFromPEM takes it happily.
func TestServerAuthoritiesRefusesTheServersOwnCertificate(t *testing.T) {
	now := time.Now()
	template := serverAuthorityTemplate("ra8ci-server", now.Add(-time.Hour), now.Add(time.Hour))
	template.IsCA = false
	template.KeyUsage = x509.KeyUsageDigitalSignature
	bundle, leaf := serverAuthorityPEM(t, template)
	if !x509.NewCertPool().AppendCertsFromPEM(bundle) {
		t.Fatal("the standard pool refused a leaf certificate; this test is pinning the wrong thing")
	}
	_, err := ServerAuthorities(bundle, now)
	if !errors.Is(err, ErrIdentity) || !strings.Contains(err.Error(), "is not a certificate authority") {
		t.Fatalf("an end-entity certificate in the server CA bundle was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), Fingerprint(leaf)) || !strings.Contains(err.Error(), "ra8ci-server") {
		t.Fatalf("the refusal does not name which certificate it refused: %v", err)
	}
}

func TestServerAuthoritiesRefusesAnAuthorityThatMayNotSign(t *testing.T) {
	now := time.Now()
	template := serverAuthorityTemplate("ra8ci-server-ca-nosign", now.Add(-time.Hour), now.Add(time.Hour))
	template.KeyUsage = x509.KeyUsageCRLSign
	bundle, _ := serverAuthorityPEM(t, template)
	_, err := ServerAuthorities(bundle, now)
	if !errors.Is(err, ErrIdentity) || !strings.Contains(err.Error(), "may not sign certificates") {
		t.Fatalf("a non-signing server authority was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "server CA bundle") {
		t.Fatalf("the refusal does not name which bundle it read: %v", err)
	}
}

func TestServerAuthoritiesRefusesABundleThatHasEntirelyExpired(t *testing.T) {
	now := time.Now()
	bundle, _ := serverAuthorityPEM(t, serverAuthorityTemplate("ra8ci-server-ca-old", now.Add(-48*time.Hour), now.Add(-time.Hour)))
	_, err := ServerAuthorities(bundle, now)
	if !errors.Is(err, ErrIdentity) || !strings.Contains(err.Error(), "validity window") {
		t.Fatalf("a bundle of expired server authorities was accepted: %v", err)
	}
}

// A retiring authority beside a live one is a rotation, not a fault: clients
// still have to verify servers whose certificate the old authority issued.
func TestServerAuthoritiesAcceptsARetiringAuthorityBesideALiveOne(t *testing.T) {
	now := time.Now()
	retiring, _ := serverAuthorityPEM(t, serverAuthorityTemplate("ra8ci-server-ca-old", now.Add(-48*time.Hour), now.Add(-time.Hour)))
	live, _ := serverAuthorityPEM(t, serverAuthorityTemplate("ra8ci-server-ca-new", now.Add(-time.Hour), now.Add(48*time.Hour)))
	if _, err := ServerAuthorities(append(retiring, live...), now); err != nil {
		t.Fatalf("a CA rotation was refused: %v", err)
	}
}

// Both ends read one rule. A bundle refused for the listener is refused for a
// client too, and the message says which file the operator should go and look
// at rather than which end of the connection the code happened to be on.
func TestBothBundlesAreJudgedByTheSameRuleAndNameTheirOwnSide(t *testing.T) {
	now := time.Now()
	template := serverAuthorityTemplate("ra8ci-ca-nosign", now.Add(-time.Hour), now.Add(time.Hour))
	template.KeyUsage = x509.KeyUsageCRLSign
	bundle, _ := serverAuthorityPEM(t, template)
	serverErr := errorText(ServerAuthorities(bundle, now))
	clientErr := errorText(ClientAuthorities(bundle, now))
	if serverErr == "" || clientErr == "" {
		t.Fatalf("one end accepted what the other refused: server=%q client=%q", serverErr, clientErr)
	}
	if !strings.Contains(serverErr, "server CA bundle") || !strings.Contains(clientErr, "client CA bundle") {
		t.Fatalf("a refusal does not name its own bundle: server=%q client=%q", serverErr, clientErr)
	}
}

func errorText(_ *x509.CertPool, err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}
