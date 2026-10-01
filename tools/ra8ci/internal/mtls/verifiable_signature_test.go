package mtls

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"strconv"
	"strings"
	"testing"
	"time"
)

// signedWith issues a real certificate from the given template and then states
// a different signature algorithm on the parsed leaf. A certificate signed with
// MD5 or SHA-1 cannot be minted by this toolchain at all, which is the whole
// reason the rule exists: the certificates carrying these signatures were
// issued years ago by something else and arrive here as bytes on disk.
func signedWith(t *testing.T, template *x509.Certificate, algorithm x509.SignatureAlgorithm) tls.Certificate {
	t.Helper()
	identity := issue(t, template)
	leaf, err := x509.ParseCertificate(identity.Certificate[0])
	if err != nil {
		t.Fatalf("parse leaf: %v", err)
	}
	leaf.SignatureAlgorithm = algorithm
	identity.Leaf = leaf
	return identity
}

// signingAuthority is a parent the toolchain's own verifier will get as far as
// the signature with: an authority, allowed to sign, holding a real key.
func signingAuthority(t *testing.T) *x509.Certificate {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	return &x509.Certificate{
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign,
		PublicKeyAlgorithm:    x509.ECDSA,
		PublicKey:             &key.PublicKey,
	}
}

func TestAModernSignatureIsAccepted(t *testing.T) {
	for name, algorithm := range map[string]x509.SignatureAlgorithm{
		"ECDSA with SHA-256":   x509.ECDSAWithSHA256,
		"ECDSA with SHA-384":   x509.ECDSAWithSHA384,
		"RSA with SHA-256":     x509.SHA256WithRSA,
		"RSA-PSS with SHA-256": x509.SHA256WithRSAPSS,
		"Ed25519":              x509.PureEd25519,
	} {
		certificate := &x509.Certificate{SignatureAlgorithm: algorithm}
		if err := checkSignatureIsVerifiable(certificate, "subject", "as a client identity"); err != nil {
			t.Fatalf("%s was refused: %v", name, err)
		}
	}
	// And the whole way through, on a certificate this toolchain really signed.
	if err := ValidateClientIdentity(issue(t, clientTemplate()), testNow); err != nil {
		t.Fatalf("a certificate signed by this toolchain was refused: %v", err)
	}
}

func TestEveryRefusedAlgorithmIsNamedInTheRefusal(t *testing.T) {
	for _, algorithm := range []x509.SignatureAlgorithm{
		x509.MD2WithRSA,
		x509.MD5WithRSA,
		x509.SHA1WithRSA,
		x509.DSAWithSHA1,
		x509.ECDSAWithSHA1,
	} {
		certificate := &x509.Certificate{SignatureAlgorithm: algorithm}
		err := checkSignatureIsVerifiable(certificate, "subject \"ra8ci-client\" sha256 abc", "as a client identity")
		if err == nil {
			t.Fatalf("%s was accepted", algorithm)
		}
		if !errors.Is(err, ErrIdentity) {
			t.Fatalf("%s: refusal is not ErrIdentity: %v", algorithm, err)
		}
		if !strings.Contains(err.Error(), algorithm.String()) {
			t.Fatalf("%s: the refusal does not name the algorithm: %v", algorithm, err)
		}
		if !strings.Contains(err.Error(), "as a client identity") {
			t.Fatalf("%s: the refusal does not name where the certificate was found: %v", algorithm, err)
		}
	}
}

func TestACertificateWithNoIdentifiableAlgorithmIsRefused(t *testing.T) {
	err := checkSignatureIsVerifiable(&x509.Certificate{}, "subject", "as a client identity")
	if err == nil || !strings.Contains(err.Error(), "no verifier on this connection can identify") {
		t.Fatalf("expected an unidentifiable-algorithm refusal, got %v", err)
	}
}

// The cross-check: the set refused here is the set the verifier this process
// links against refuses outright, asked of the verifier itself rather than
// transcribed from its documentation. CheckSignatureFrom is the call the path
// builder makes for every link it walks, and it answers an algorithm it will
// never accept with InsecureAlgorithmError or ErrUnsupportedAlgorithm before it
// looks at the key. Any other answer, a plain verification failure included,
// means the algorithm itself was fine and the question belongs to the far end.
func TestTheRuleMatchesTheVerifierThisProcessLinksAgainst(t *testing.T) {
	parent := signingAuthority(t)
	decided := 0
	for value := 0; value <= 32; value++ {
		algorithm := x509.SignatureAlgorithm(value)
		if value != 0 && algorithm.String() == strconv.Itoa(value) {
			continue // not an algorithm this toolchain knows about
		}
		decided++
		child := &x509.Certificate{SignatureAlgorithm: algorithm}
		verifierAnswer := child.CheckSignatureFrom(parent)
		var insecure x509.InsecureAlgorithmError
		refusedOutright := errors.As(verifierAnswer, &insecure) || errors.Is(verifierAnswer, x509.ErrUnsupportedAlgorithm)
		weRefuse := checkSignatureIsVerifiable(child, "subject", "as a client identity") != nil
		if refusedOutright != weRefuse {
			t.Fatalf("%s: the verifier refuses it outright %v, this package refuses it %v (verifier said %v)",
				algorithm, refusedOutright, weRefuse, verifierAnswer)
		}
	}
	if decided < 10 {
		t.Fatalf("only %d algorithms were decided; the sweep is not reaching the toolchain's table", decided)
	}
}

func TestALeafSignedWithARefusedAlgorithmIsNotPresented(t *testing.T) {
	err := ValidateClientIdentity(signedWith(t, clientTemplate(), x509.SHA1WithRSA), testNow)
	if err == nil || !strings.Contains(err.Error(), "SHA1-RSA") {
		t.Fatalf("expected a signature refusal naming SHA1-RSA, got %v", err)
	}
	if !strings.Contains(err.Error(), "as a client identity") {
		t.Fatalf("the refusal does not say where the certificate was found: %v", err)
	}
}

func TestTheServerIdentityIsHeldToTheSameSignatureRule(t *testing.T) {
	server := clientTemplate()
	server.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
	err := ValidateServerIdentity(signedWith(t, server, x509.MD5WithRSA), testNow)
	if err == nil || !strings.Contains(err.Error(), "MD5-RSA") {
		t.Fatalf("expected a signature refusal naming MD5-RSA, got %v", err)
	}
	if !strings.Contains(err.Error(), "as a server identity") {
		t.Fatalf("the refusal does not say where the certificate was found: %v", err)
	}
}

func TestAnIntermediateSignedWithARefusedAlgorithmIsRefusedNamingItsPosition(t *testing.T) {
	issuer := chainAuthority()
	issuer.SignatureAlgorithm = x509.ECDSAWithSHA1
	err := checkChainLink(issuer, 1, "client", testNow)
	if err == nil || !strings.Contains(err.Error(), "ECDSA-SHA1") {
		t.Fatalf("expected a signature refusal naming ECDSA-SHA1, got %v", err)
	}
	if !strings.Contains(err.Error(), "position 1 of the presented client chain") {
		t.Fatalf("the refusal does not name the position: %v", err)
	}
}

// The order this pins: a signature nothing will verify is a permanent property
// of the certificate, so it is reported ahead of the clock. An operator holding
// a certificate that is both expired and signed with SHA-1 has to reissue it
// either way, and the algorithm is the part a renewal alone will not fix.
func TestTheSignatureIsDecidedBeforeTheValidityWindow(t *testing.T) {
	expired := clientTemplate()
	expired.NotBefore = testNow.Add(-48 * time.Hour)
	expired.NotAfter = testNow.Add(-time.Hour)
	err := ValidateClientIdentity(signedWith(t, expired, x509.SHA1WithRSA), testNow)
	if err == nil || !strings.Contains(err.Error(), "SHA1-RSA") {
		t.Fatalf("expected the signature refusal first, got %v", err)
	}
	if strings.Contains(err.Error(), "expired at") {
		t.Fatalf("the expiry was reported instead of the signature: %v", err)
	}
}

// Separation: the same fixture with a signature the far end will verify is
// accepted, so the refusals above are about the algorithm and nothing else.
func TestOnlyTheAlgorithmDecidesThisRule(t *testing.T) {
	if err := ValidateClientIdentity(signedWith(t, clientTemplate(), x509.ECDSAWithSHA256), testNow); err != nil {
		t.Fatalf("the same fixture with a modern signature was refused: %v", err)
	}
	// A trust bundle is deliberately left out of this rule: a root's own
	// self-signature is never verified by the far end.
	authority := chainAuthority()
	authority.SignatureAlgorithm = x509.SHA1WithRSA
	if err := checkAuthorityCanSign(authority, "subject", "client"); err != nil {
		t.Fatalf("a trusted authority was refused for its own self-signature: %v", err)
	}
}
