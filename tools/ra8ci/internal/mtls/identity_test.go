package mtls

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"errors"
	"math/big"
	"strings"
	"testing"
	"time"
)

var testNow = time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)

func issue(t *testing.T, template *x509.Certificate) tls.Certificate {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	template.SerialNumber = big.NewInt(1)
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}
}

func clientTemplate() *x509.Certificate {
	return &x509.Certificate{
		Subject:               pkix.Name{CommonName: "ra8ci-client"},
		NotBefore:             testNow.Add(-time.Hour),
		NotAfter:              testNow.Add(time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
		BasicConstraintsValid: true,
	}
}

func TestValidClientIdentityIsAccepted(t *testing.T) {
	if err := ValidateClientIdentity(issue(t, clientTemplate()), testNow); err != nil {
		t.Fatalf("a client certificate was refused: %v", err)
	}
	// A certificate that declares no extended key usage is unconstrained and
	// must keep working; refusing it would lock out a running deployment.
	plain := clientTemplate()
	plain.ExtKeyUsage = nil
	plain.KeyUsage = 0
	if err := ValidateClientIdentity(issue(t, plain), testNow); err != nil {
		t.Fatalf("an unconstrained certificate was refused: %v", err)
	}
}

func TestExpiredAndNotYetValidAreRefusedLocally(t *testing.T) {
	expired := clientTemplate()
	expired.NotBefore = testNow.Add(-48 * time.Hour)
	expired.NotAfter = testNow.Add(-time.Hour)
	err := ValidateClientIdentity(issue(t, expired), testNow)
	if err == nil || !strings.Contains(err.Error(), "expired at") {
		t.Fatalf("expected an expiry refusal, got %v", err)
	}
	early := clientTemplate()
	early.NotBefore = testNow.Add(time.Hour)
	early.NotAfter = testNow.Add(48 * time.Hour)
	err = ValidateClientIdentity(issue(t, early), testNow)
	if err == nil || !strings.Contains(err.Error(), "not valid until") {
		t.Fatalf("expected a not-yet-valid refusal, got %v", err)
	}
}

func TestServerOnlyCertificateIsRefused(t *testing.T) {
	serverOnly := clientTemplate()
	serverOnly.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
	err := ValidateClientIdentity(issue(t, serverOnly), testNow)
	if err == nil || !strings.Contains(err.Error(), "client authentication") {
		t.Fatalf("expected a key-usage refusal, got %v", err)
	}
	any := clientTemplate()
	any.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageAny}
	if err := ValidateClientIdentity(issue(t, any), testNow); err != nil {
		t.Fatalf("ExtKeyUsageAny was refused: %v", err)
	}
}

// Presenting the authority's own certificate means its private key is on this
// host. That is a misconfiguration worth naming locally rather than handing to
// the TLS stack.
func TestCertificateAuthorityIsRefusedAsAnIdentity(t *testing.T) {
	authority := clientTemplate()
	authority.IsCA = true
	authority.KeyUsage = x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign
	err := ValidateClientIdentity(issue(t, authority), testNow)
	if err == nil || !strings.Contains(err.Error(), "certificate authority") {
		t.Fatalf("expected a CA refusal, got %v", err)
	}
}

func TestSigningKeyUsageIsRequiredWhenDeclared(t *testing.T) {
	noSign := clientTemplate()
	noSign.KeyUsage = x509.KeyUsageKeyEncipherment
	err := ValidateClientIdentity(issue(t, noSign), testNow)
	if err == nil || !strings.Contains(err.Error(), "may not be used to sign") {
		t.Fatalf("expected a signing refusal, got %v", err)
	}
}

func TestEveryRefusalIsErrIdentityAndNamesTheFingerprint(t *testing.T) {
	expired := clientTemplate()
	expired.NotAfter = testNow.Add(-time.Hour)
	expired.NotBefore = testNow.Add(-48 * time.Hour)
	identity := issue(t, expired)
	err := ValidateClientIdentity(identity, testNow)
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("refusal is not ErrIdentity: %v", err)
	}
	leaf, parseErr := Leaf(identity)
	if parseErr != nil {
		t.Fatalf("leaf: %v", parseErr)
	}
	print := Fingerprint(leaf)
	if len(print) != 64 || !strings.Contains(err.Error(), print) {
		t.Fatalf("refusal %q does not name fingerprint %q", err, print)
	}
	if !strings.Contains(err.Error(), "ra8ci-client") {
		t.Fatalf("refusal does not name the subject: %v", err)
	}
}

func TestEmptyKeyPairIsRefused(t *testing.T) {
	if err := ValidateClientIdentity(tls.Certificate{}, testNow); !errors.Is(err, ErrIdentity) {
		t.Fatalf("an empty key pair was not refused: %v", err)
	}
	if got := Fingerprint(nil); got != "" {
		t.Fatalf("Fingerprint(nil)=%q", got)
	}
}

func serverTemplate() *x509.Certificate {
	return &x509.Certificate{
		Subject:               pkix.Name{CommonName: "ra8ci-server"},
		NotBefore:             testNow.Add(-time.Hour),
		NotAfter:              testNow.Add(time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
}

func TestValidServerIdentityIsAccepted(t *testing.T) {
	if err := ValidateServerIdentity(issue(t, serverTemplate()), testNow); err != nil {
		t.Fatalf("a server certificate was refused: %v", err)
	}
	plain := serverTemplate()
	plain.ExtKeyUsage = nil
	plain.KeyUsage = 0
	if err := ValidateServerIdentity(issue(t, plain), testNow); err != nil {
		t.Fatalf("an unconstrained certificate was refused: %v", err)
	}
	any := serverTemplate()
	any.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageAny}
	if err := ValidateServerIdentity(issue(t, any), testNow); err != nil {
		t.Fatalf("ExtKeyUsageAny was refused: %v", err)
	}
}

// The listener and the clients must not be able to swap identities: a client
// certificate served from the listener is a misconfiguration that otherwise
// surfaces as every client failing to connect.
func TestClientOnlyCertificateIsRefusedAsAServerIdentity(t *testing.T) {
	err := ValidateServerIdentity(issue(t, clientTemplate()), testNow)
	if err == nil || !strings.Contains(err.Error(), "server authentication") {
		t.Fatalf("expected a key-usage refusal, got %v", err)
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("refusal is not ErrIdentity: %v", err)
	}
	if err := ValidateClientIdentity(issue(t, serverTemplate()), testNow); err == nil {
		t.Fatal("a server certificate was accepted as a client identity")
	}
}

func TestServerIdentityRefusalsNameTheCertificateAndNotTheKey(t *testing.T) {
	expired := serverTemplate()
	expired.NotBefore = testNow.Add(-48 * time.Hour)
	expired.NotAfter = testNow.Add(-time.Hour)
	identity := issue(t, expired)
	err := ValidateServerIdentity(identity, testNow)
	if err == nil || !strings.Contains(err.Error(), "expired at") {
		t.Fatalf("expected an expiry refusal, got %v", err)
	}
	leaf, parseErr := Leaf(identity)
	if parseErr != nil {
		t.Fatalf("leaf: %v", parseErr)
	}
	if !strings.Contains(err.Error(), Fingerprint(leaf)) || !strings.Contains(err.Error(), "ra8ci-server") {
		t.Fatalf("refusal %q does not name the subject and fingerprint", err)
	}

	early := serverTemplate()
	early.NotBefore = testNow.Add(time.Hour)
	early.NotAfter = testNow.Add(48 * time.Hour)
	if err := ValidateServerIdentity(issue(t, early), testNow); err == nil || !strings.Contains(err.Error(), "not valid until") {
		t.Fatalf("expected a not-yet-valid refusal, got %v", err)
	}

	authority := serverTemplate()
	authority.IsCA = true
	authority.KeyUsage = x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign
	if err := ValidateServerIdentity(issue(t, authority), testNow); err == nil || !strings.Contains(err.Error(), "certificate authority") {
		t.Fatalf("expected a CA refusal, got %v", err)
	}

	noSign := serverTemplate()
	noSign.KeyUsage = x509.KeyUsageKeyEncipherment
	if err := ValidateServerIdentity(issue(t, noSign), testNow); err == nil || !strings.Contains(err.Error(), "may not be used to sign") {
		t.Fatalf("expected a signing refusal, got %v", err)
	}

	if err := ValidateServerIdentity(tls.Certificate{}, testNow); !errors.Is(err, ErrIdentity) {
		t.Fatalf("an empty key pair was not refused: %v", err)
	}
}

func authorityTemplate(commonName string) *x509.Certificate {
	return &x509.Certificate{
		Subject:               pkix.Name{CommonName: commonName},
		NotBefore:             testNow.Add(-24 * time.Hour),
		NotAfter:              testNow.Add(24 * time.Hour),
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
		IsCA:                  true,
		BasicConstraintsValid: true,
	}
}

func encodePEM(t *testing.T, identity tls.Certificate) []byte {
	t.Helper()
	return pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: identity.Certificate[0]})
}

func TestClientAuthoritiesAcceptsABundleThatCanVerifySomebody(t *testing.T) {
	bundle := encodePEM(t, issue(t, authorityTemplate("ra8ci-client-ca")))
	pool, err := ClientAuthorities(bundle, testNow)
	if err != nil {
		t.Fatalf("a live authority was refused: %v", err)
	}
	if pool == nil {
		t.Fatal("no pool returned")
	}

	// A CA rotation leaves the retiring authority in the bundle while the
	// certificates it issued are still being replaced. That is not an error.
	retiring := authorityTemplate("ra8ci-client-ca-old")
	retiring.NotBefore = testNow.Add(-72 * time.Hour)
	retiring.NotAfter = testNow.Add(-time.Hour)
	rotating := append(encodePEM(t, issue(t, retiring)), bundle...)
	if _, err := ClientAuthorities(rotating, testNow); err != nil {
		t.Fatalf("a rotating bundle was refused: %v", err)
	}
}

func TestClientAuthoritiesRefusesABundleThatCanVerifyNobody(t *testing.T) {
	if _, err := ClientAuthorities(nil, testNow); !errors.Is(err, ErrIdentity) {
		t.Fatalf("an empty bundle was not refused: %v", err)
	}
	if _, err := ClientAuthorities([]byte("not pem at all"), testNow); !errors.Is(err, ErrIdentity) {
		t.Fatalf("a bundle with no certificate was not refused: %v", err)
	}

	// An end-entity certificate in the bundle parses and pools, and then
	// verifies nothing. AppendCertsFromPEM reports success for it.
	leafOnly := encodePEM(t, issue(t, clientTemplate()))
	_, err := ClientAuthorities(leafOnly, testNow)
	if err == nil || !strings.Contains(err.Error(), "is not a certificate authority") {
		t.Fatalf("expected a not-an-authority refusal, got %v", err)
	}
	if x509.NewCertPool().AppendCertsFromPEM(leafOnly) != true {
		t.Fatal("the standard pool no longer accepts a leaf, this test is pinning the wrong thing")
	}

	expired := authorityTemplate("ra8ci-client-ca-expired")
	expired.NotBefore = testNow.Add(-72 * time.Hour)
	expired.NotAfter = testNow.Add(-time.Hour)
	_, err = ClientAuthorities(encodePEM(t, issue(t, expired)), testNow)
	if err == nil || !strings.Contains(err.Error(), "validity window") {
		t.Fatalf("expected an expiry refusal, got %v", err)
	}
}
