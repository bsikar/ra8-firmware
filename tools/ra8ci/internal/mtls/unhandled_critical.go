// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/x509"
	"fmt"
	"strings"
)

// checkNoUnhandledCriticalExtension refuses a certificate carrying a critical
// extension the verifier at the far end cannot interpret.
//
// A critical extension is the issuer saying this certificate must not be used
// by anything that does not understand this field. x509.ParseCertificate
// honours that by parsing the certificate anyway and recording every critical
// extension it did not process in UnhandledCriticalExtensions; nothing in this
// package read it. So a leaf carrying a critical policy constraint, a critical
// extension from a corporate profile, or anything else Go does not process
// loaded, passed every rule here, and was presented on every handshake.
//
// The far end is where it fails, and it fails as the same opaque denial this
// package exists to name first: Certificate.Verify refuses a certificate with
// unhandled critical extensions before it looks at anything else, at the leaf
// and at every certificate on the path it builds. What reaches the operator is
// a handshake failure against a certificate whose subject, window, usages and
// authority are all correct, and the field that actually stopped it is not
// named anywhere on the host that presented it.
//
// The rule is permanent rather than a reading of the clock: an extension this
// process cannot interpret will not start being interpretable, so it is
// refused outright the way a certificate that may not sign is, and not given
// the tolerance an expired authority in a rotation gets.
//
// where names the subject and the public fingerprint, at names the place the
// certificate was found, and the message lists the extensions by OID because
// the OID is the only thing that tells an operator which field of which
// profile to go and look at.
func checkNoUnhandledCriticalExtension(certificate *x509.Certificate, where, at string) error {
	if certificate == nil || len(certificate.UnhandledCriticalExtensions) == 0 {
		return nil
	}
	return fmt.Errorf("%w: %s %s carries critical extension %s that no verifier on this connection can interpret",
		ErrIdentity, where, at, unhandledExtensionList(certificate))
}

// unhandledExtensionList names the extensions in the order the certificate
// carries them, so the list reads in the same order as the certificate an
// operator opens beside it.
func unhandledExtensionList(certificate *x509.Certificate) string {
	names := make([]string, 0, len(certificate.UnhandledCriticalExtensions))
	for _, extension := range certificate.UnhandledCriticalExtensions {
		names = append(names, extension.String())
	}
	return strings.Join(names, ", ")
}
