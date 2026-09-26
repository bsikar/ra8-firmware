// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/x509"
	"fmt"
)

// checkSignatureIsVerifiable refuses a certificate this process is about to
// present whose signature the verifier at the far end will not accept at all,
// whatever else is right about it.
//
// Every other rule in this package reads what a certificate SAYS: what it is,
// what it may be used for, when it is good for. This reads how it was SIGNED,
// which is the one property of a presented certificate that the far end judges
// before it judges anything else and that nothing on this host ever looks at.
// tls.LoadX509KeyPair parses the file and matches the key to it; x509.Parse-
// Certificate records the signature algorithm in SignatureAlgorithm and
// verifies nothing. So a leaf or an intermediate signed under an old corporate
// profile, or by an authority whose signing profile was never modernised,
// loads here without complaint and is presented on every handshake.
//
// The far end is where it fails, and it fails as the same opaque denial the
// rest of this package exists to name first. The verifier this tree links
// against refuses these signatures before it checks the key: checkSignature
// answers an MD5 or SHA-1 signature with InsecureAlgorithmError, and an
// algorithm it does not implement, which is what MD2, DSA and an unrecognised
// algorithm identifier all reach, with ErrUnsupportedAlgorithm. Either one is
// wrapped by the path builder and surfaces as "certificate signed by unknown
// authority", so what arrives at the bench is a certificate whose subject,
// window, usages and authority are all correct being refused for a reason
// named nowhere on the host that presented it.
//
// The rule is permanent rather than a reading of the clock, so it sits beside
// the unhandled-critical-extension rule and is decided before the validity
// window: a signature algorithm does not become acceptable later, and a
// certificate that cannot be verified is worth refusing by the thing that is
// actually wrong with it rather than by whichever rule happens to trip first.
//
// DSA with SHA-256 is deliberately absent, and the test that holds this set
// against the verifier is what settled it: the hash is one the verifier
// computes, so it gets as far as the key and answers a mismatch rather than
// refusing the algorithm outright. What happens to such a certificate depends
// on the authority verifying it, which makes it the far end's question and not
// a permanent property decidable here. DSA with SHA-1 is refused, but for the
// SHA-1, like every other SHA-1 signature.
//
// It is deliberately not applied to the certificate authorities in a trust
// bundle. Those are used as roots, and a root's own self-signature is never
// verified by the far end, so refusing one for the algorithm it signed itself
// with would lock out a working deployment to say nothing about any handshake.
//
// where names the subject and the public fingerprint, at names the place the
// certificate was found, and the message names the algorithm because that is
// the only thing that tells an operator which certificate in which profile to
// go and have reissued.
func checkSignatureIsVerifiable(certificate *x509.Certificate, where, at string) error {
	if certificate == nil {
		return nil
	}
	algorithm := certificate.SignatureAlgorithm
	if algorithm == x509.UnknownSignatureAlgorithm {
		return fmt.Errorf("%w: %s %s is signed with an algorithm no verifier on this connection can identify",
			ErrIdentity, where, at)
	}
	if reason, refused := unverifiableSignatures[algorithm]; refused {
		return fmt.Errorf("%w: %s %s is signed with %s, which no verifier on this connection %s",
			ErrIdentity, where, at, algorithm, reason)
	}
	return nil
}

// unverifiableSignatures is the set of signature algorithms the verifier at
// the far end refuses outright, and the reason it gives, stated here so the
// refusal on this side reads as the same finding rather than a near miss of
// it. Everything absent from this map is verified on its merits, which is the
// right default: a signature this host cannot judge is the far end's question,
// and only an algorithm that can never be verified is the local one.
var unverifiableSignatures = map[x509.SignatureAlgorithm]string{
	x509.MD2WithRSA:    "implements",
	x509.MD5WithRSA:    "accepts",
	x509.SHA1WithRSA:   "accepts",
	x509.DSAWithSHA1:   "accepts",
	x509.ECDSAWithSHA1: "accepts",
}
