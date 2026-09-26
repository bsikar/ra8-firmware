package mtls

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/asn1"
	"encoding/pem"
	"errors"
	"math/big"
	"strings"
	"testing"
)

// criticalExtension is an extension from some profile this process does not
// implement, marked critical by its issuer: the shape x509.ParseCertificate
// records in UnhandledCriticalExtensions.
func criticalExtension(oid string) pkix.Extension {
	return pkix.Extension{
		Id:       parseOID(oid),
		Critical: true,
		// An ASN.1 NULL: parseable, and meaningless to anything that does
		// not know the profile it comes from.
		Value: []byte{0x05, 0x00},
	}
}

func parseOID(oid string) asn1.ObjectIdentifier {
	var parsed asn1.ObjectIdentifier
	for _, part := range strings.Split(oid, ".") {
		value := 0
		for _, digit := range part {
			value = value*10 + int(digit-'0')
		}
		parsed = append(parsed, value)
	}
	return parsed
}

// signedFor mints a certificate under an authority rather than under itself,
// so a real verifier can be asked to walk the same certificate this rule
// judges.
func signedFor(t *testing.T, template, parent *x509.Certificate, parentKey *ecdsa.PrivateKey) *x509.Certificate {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	template.SerialNumber = big.NewInt(7)
	der, err := x509.CreateCertificate(rand.Reader, template, parent, &key.PublicKey, parentKey)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	certificate, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parse certificate: %v", err)
	}
	return certificate
}

func TestAnIdentityCarryingNothingUnhandledIsStillAccepted(t *testing.T) {
	if err := ValidateClientIdentity(issue(t, clientTemplate()), testNow); err != nil {
		t.Fatalf("an ordinary client identity was refused: %v", err)
	}
	if err := ValidateServerIdentity(issue(t, serverTemplate()), testNow); err != nil {
		t.Fatalf("an ordinary server identity was refused: %v", err)
	}
}

func TestACriticalExtensionThisProcessCannotInterpretIsRefused(t *testing.T) {
	template := clientTemplate()
	template.ExtraExtensions = []pkix.Extension{criticalExtension("1.3.6.1.4.1.99999.1")}
	err := ValidateClientIdentity(issue(t, template), testNow)
	if err == nil || !strings.Contains(err.Error(), "1.3.6.1.4.1.99999.1") {
		t.Fatalf("expected a refusal naming the extension, got %v", err)
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("refusal is not an identity error: %v", err)
	}
	if !strings.Contains(err.Error(), "as a client identity") {
		t.Fatalf("refusal does not name where the certificate was found: %v", err)
	}
}

func TestTheSameRuleHoldsAServerIdentity(t *testing.T) {
	template := serverTemplate()
	template.ExtraExtensions = []pkix.Extension{criticalExtension("1.3.6.1.4.1.99999.2")}
	err := ValidateServerIdentity(issue(t, template), testNow)
	if err == nil || !strings.Contains(err.Error(), "1.3.6.1.4.1.99999.2") {
		t.Fatalf("expected a refusal naming the extension, got %v", err)
	}
	if !strings.Contains(err.Error(), "as a server identity") {
		t.Fatalf("refusal does not name where the certificate was found: %v", err)
	}
}

// An extension that is NOT critical is the issuer saying it may be ignored,
// which is what every verifier on the connection does. Refusing one would lock
// out certificates that work.
func TestAnExtensionThatWasNotMarkedCriticalIsAccepted(t *testing.T) {
	template := clientTemplate()
	passive := criticalExtension("1.3.6.1.4.1.99999.3")
	passive.Critical = false
	template.ExtraExtensions = []pkix.Extension{passive}
	if err := ValidateClientIdentity(issue(t, template), testNow); err != nil {
		t.Fatalf("a non-critical unknown extension was refused: %v", err)
	}
}

func TestAPresentedChainLinkIsHeldToTheSameRule(t *testing.T) {
	template := chainAuthority()
	template.ExtraExtensions = []pkix.Extension{criticalExtension("1.3.6.1.4.1.99999.4")}
	err := ValidateClientIdentity(presenting(t, link(t, template)), testNow)
	if err == nil || !strings.Contains(err.Error(), "1.3.6.1.4.1.99999.4") {
		t.Fatalf("expected a refusal naming the extension, got %v", err)
	}
	if !strings.Contains(err.Error(), "at position 1 of the presented client chain") {
		t.Fatalf("refusal does not name the position in the chain: %v", err)
	}
}

func TestACertificateAuthorityBundleIsHeldToTheSameRule(t *testing.T) {
	template := authorityTemplate("ra8ci-unreadable-authority")
	template.ExtraExtensions = []pkix.Extension{criticalExtension("1.3.6.1.4.1.99999.5")}
	bundle := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: issue(t, template).Certificate[0]})
	for _, authorities := range []struct {
		role  string
		parse func([]byte) (*x509.CertPool, error)
	}{
		{"client", func(b []byte) (*x509.CertPool, error) { return ClientAuthorities(b, testNow) }},
		{"server", func(b []byte) (*x509.CertPool, error) { return ServerAuthorities(b, testNow) }},
	} {
		_, err := authorities.parse(bundle)
		if err == nil || !strings.Contains(err.Error(), "1.3.6.1.4.1.99999.5") {
			t.Fatalf("%s bundle: expected a refusal naming the extension, got %v", authorities.role, err)
		}
		if !strings.Contains(err.Error(), "in the "+authorities.role+" CA bundle") {
			t.Fatalf("%s bundle: refusal does not name the bundle: %v", authorities.role, err)
		}
	}
}

// Every extension the certificate carried, in the order it carried them: an
// operator reading the refusal has the same order as the certificate they open
// beside it.
func TestTheRefusalNamesEveryExtensionItCouldNotInterpret(t *testing.T) {
	template := clientTemplate()
	template.ExtraExtensions = []pkix.Extension{
		criticalExtension("1.3.6.1.4.1.99999.6"),
		criticalExtension("1.3.6.1.4.1.99999.7"),
	}
	err := ValidateClientIdentity(issue(t, template), testNow)
	if err == nil {
		t.Fatal("expected a refusal")
	}
	if !strings.Contains(err.Error(), "1.3.6.1.4.1.99999.6, 1.3.6.1.4.1.99999.7") {
		t.Fatalf("refusal does not list both extensions in order: %v", err)
	}
}

// The premise, pinned rather than asserted in a comment: the verifier at the
// far end refuses exactly what this rule refuses, and refuses it as a path
// failure that says nothing about the subject or the window.
func TestTheFarEndRefusesExactlyWhatThisRuleRefuses(t *testing.T) {
	rootKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	rootTemplate := authorityTemplate("ra8ci-root")
	rootTemplate.SerialNumber = big.NewInt(6)
	rootDER, err := x509.CreateCertificate(rand.Reader, rootTemplate, rootTemplate, &rootKey.PublicKey, rootKey)
	if err != nil {
		t.Fatalf("create root: %v", err)
	}
	root, err := x509.ParseCertificate(rootDER)
	if err != nil {
		t.Fatalf("parse root: %v", err)
	}
	leafTemplate := clientTemplate()
	leafTemplate.ExtraExtensions = []pkix.Extension{criticalExtension("1.3.6.1.4.1.99999.8")}
	leaf := signedFor(t, leafTemplate, root, rootKey)

	_, err = leaf.Verify(x509.VerifyOptions{
		Roots:       poolOf(root),
		CurrentTime: testNow,
		KeyUsages:   []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	})
	var unhandled x509.UnhandledCriticalExtension
	if !errors.As(err, &unhandled) {
		t.Fatalf("expected the verifier to refuse the certificate for its critical extension, got %v", err)
	}
	// And the same certificate, judged here, is refused before it is ever
	// presented, naming the field the verifier does not.
	local := ValidateClientIdentity(tls.Certificate{Certificate: [][]byte{leaf.Raw}}, testNow)
	if local == nil || !strings.Contains(local.Error(), "1.3.6.1.4.1.99999.8") {
		t.Fatalf("expected the local refusal to name the extension, got %v", local)
	}
}
