// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"bytes"
	"crypto/tls"
	"crypto/x509"
	"fmt"
)

// checkPresentedChainIsAPath holds the certificates a key pair presents beside
// its leaf to being a PATH: each one certifies the one before it.
//
// checkPresentedChain judges every certificate beside the leaf on its own
// properties, an authority, allowed to sign, readable, usable today, and its
// own doc says what those properties are for: "A presented chain is a PATH, not
// a bundle of alternatives". Nothing ever asked whether the certificates in it
// actually form one. A file holding a good leaf next to a good intermediate
// from a DIFFERENT authority, the ordinary result of assembling a chain by
// concatenating whatever PEM the operator had to hand, passes every existing
// check and is presented on every handshake.
//
// RFC 8446 section 4.4.2 is explicit that each certificate in the list must
// directly certify the one preceding it. A far end holding the real root
// cannot build a path out of these: the leaf names an issuer no certificate in
// the message is, so the verifier stops with "certificate signed by unknown
// authority". That is the denial this package exists to end, reported from a
// host whose own leaf is perfectly good and whose error says nothing about the
// second certificate in the file.
//
// Two readings, because neither implies the other. The names have to line up,
// since Go's CheckSignatureFrom compares key identifiers and never subject
// names, so two authorities that share a key pair under different names
// satisfy the signature and still do not form the path the far end walks. And
// the signature has to verify, since a name match alone is satisfied by any
// authority that happens to be called the same thing, which is what a chain
// assembled from a stale copy of somebody else's bundle looks like.
//
// It runs after checkPresentedChain, not instead of it: a link that is not a
// usable authority at all is the more basic complaint and the operator should
// read that one first, and CheckSignatureFrom refuses a non-authority parent
// on its own terms, which would report a path failure for what is really a
// wrong certificate.
func checkPresentedChainIsAPath(identity tls.Certificate, role string) error {
	if len(identity.Certificate) < 2 {
		// A key pair sending only its leaf presents no path to be wrong
		// about. The far end builds the rest from what it already trusts.
		return nil
	}
	below, err := Leaf(identity)
	if err != nil {
		return err
	}
	for position := 1; position < len(identity.Certificate); position++ {
		issuer, err := x509.ParseCertificate(identity.Certificate[position])
		if err != nil {
			return fmt.Errorf("%w: parse the certificate at position %d of the presented %s chain: %v",
				ErrIdentity, position, role, err)
		}
		// The message names both positions, because the operator's fix is
		// to look at two certificates in one file and decide which of them
		// does not belong. It names the subject and the public fingerprint,
		// never the key, the same way every other refusal here does.
		where := fmt.Sprintf("subject %q sha256 %s", issuer.Subject.String(), Fingerprint(issuer))
		at := fmt.Sprintf("at position %d of the presented %s chain", position, role)
		if !bytes.Equal(issuer.RawSubject, below.RawIssuer) {
			return fmt.Errorf("%w: %s %s did not issue the certificate at position %d, which names issuer %q",
				ErrIdentity, where, at, position-1, below.Issuer.String())
		}
		if err := below.CheckSignatureFrom(issuer); err != nil {
			return fmt.Errorf("%w: %s %s did not sign the certificate at position %d: %v",
				ErrIdentity, where, at, position-1, err)
		}
		below = issuer
	}
	return nil
}
