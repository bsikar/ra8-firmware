package mtls

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
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
