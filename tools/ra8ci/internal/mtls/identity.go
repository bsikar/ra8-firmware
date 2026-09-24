// Package mtls states what a loaded key pair must be before a process will
// present it as its own identity on a mutual-TLS connection.
//
// The three clients in this tree (runclient, agent, boardclient) all load a
// key pair with tls.LoadX509KeyPair and hand it straight to the TLS stack.
// LoadX509KeyPair checks that the key matches the certificate and nothing
// else: an expired certificate, a certificate issued for server
// authentication, or the certificate authority's own certificate all load
// without complaint and fail later as an opaque handshake error or a denial
// the operator cannot tell apart from a missing grant.
//
// Every check here is local and decided before the first connection.
package mtls

import (
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"errors"
	"fmt"
	"time"
)

// ErrIdentity is returned for every refusal, so a caller can classify a local
// identity problem without matching on message text.
var ErrIdentity = errors.New("client identity")

// Fingerprint is the SHA-256 of a leaf's DER, the same identifier the server
// records in its audit trail and in api_principals.cert_sha256. It is derived
// from the public certificate; no private key material passes through here.
func Fingerprint(leaf *x509.Certificate) string {
	if leaf == nil || len(leaf.Raw) == 0 {
		return ""
	}
	sum := sha256.Sum256(leaf.Raw)
	return hex.EncodeToString(sum[:])
}

// Leaf parses the end-entity certificate of a loaded key pair. tls.Certificate
// leaves Leaf nil after LoadX509KeyPair, so the bytes are parsed here rather
// than by each caller.
func Leaf(identity tls.Certificate) (*x509.Certificate, error) {
	if identity.Leaf != nil {
		return identity.Leaf, nil
	}
	if len(identity.Certificate) == 0 || len(identity.Certificate[0]) == 0 {
		return nil, fmt.Errorf("%w: key pair carries no certificate", ErrIdentity)
	}
	leaf, err := x509.ParseCertificate(identity.Certificate[0])
	if err != nil {
		return nil, fmt.Errorf("%w: parse leaf certificate: %v", ErrIdentity, err)
	}
	return leaf, nil
}

// ValidateClientIdentity refuses a key pair this process must not present as
// its own client identity. It is deliberately not a substitute for the
// server's authorization: the server decides what a certificate may do, and
// this decides only that the thing being presented is a usable client
// certificate at all.
//
// The error names the subject and the public fingerprint, never the key.
func ValidateClientIdentity(identity tls.Certificate, now time.Time) error {
	leaf, err := Leaf(identity)
	if err != nil {
		return err
	}
	where := fmt.Sprintf("subject %q sha256 %s", leaf.Subject.String(), Fingerprint(leaf))
	if leaf.IsCA {
		return fmt.Errorf("%w: %s is a certificate authority, not a client identity", ErrIdentity, where)
	}
	if now.Before(leaf.NotBefore) {
		return fmt.Errorf("%w: %s is not valid until %s", ErrIdentity, where, leaf.NotBefore.UTC().Format(time.RFC3339))
	}
	if !now.Before(leaf.NotAfter) {
		return fmt.Errorf("%w: %s expired at %s", ErrIdentity, where, leaf.NotAfter.UTC().Format(time.RFC3339))
	}
	// A certificate that declares no extended key usage is unconstrained and
	// is accepted; one that declares a set must include client authentication.
	// This is what keeps a server certificate, or an identity minted for some
	// other role, from being presented here by mistake.
	if len(leaf.ExtKeyUsage) > 0 && !allowsClientAuth(leaf) {
		return fmt.Errorf("%w: %s is not issued for client authentication", ErrIdentity, where)
	}
	// TLS 1.3 client authentication is a signature made with this key. A
	// certificate that declares key usages without digital signature cannot
	// produce one.
	if leaf.KeyUsage != 0 && leaf.KeyUsage&x509.KeyUsageDigitalSignature == 0 {
		return fmt.Errorf("%w: %s may not be used to sign", ErrIdentity, where)
	}
	return nil
}

func allowsClientAuth(leaf *x509.Certificate) bool {
	for _, usage := range leaf.ExtKeyUsage {
		if usage == x509.ExtKeyUsageClientAuth || usage == x509.ExtKeyUsageAny {
			return true
		}
	}
	return false
}
