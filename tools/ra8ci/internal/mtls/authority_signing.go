// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/x509"
	"fmt"
)

// checkAuthorityCanSign refuses a certificate authority that cannot
// issue the certificates it is being trusted to have issued.
//
// An authority declaring key usages without certificate signing is a complete
// authority in every other respect: it parses, it says IsCA, it is inside its
// validity window, and x509.CertPool takes it without complaint. Verification
// is where it fails. Go's verifier holds an authority to the same rule this
// package holds an identity to, and refuses a chain through one with "parent
// certificate cannot sign this kind of certificate", surfaced to the operator
// as "certificate signed by unknown authority". That reaches the bench as
// every client being denied at once, which is indistinguishable from a
// missing grant and is exactly the failure ClientAuthorities exists to name
// before the socket opens, and the same one ServerAuthorities names before a
// client dials.
//
// An authority declaring no key usages at all is unconstrained and accepted,
// the same reading ValidateClientIdentity gives an identity with no declared
// extended key usage.
//
// This is not the expiry rule and does not share its tolerance. A retiring
// authority is a legitimate bundle member for as long as the certificates it
// issued are still being replaced, so an expired authority alongside a live
// one is a rotation rather than a fault. An authority that may not sign was
// never able to authenticate anyone and never will be, so it is refused
// outright, the same way an end-entity certificate in the bundle is.
func checkAuthorityCanSign(authority *x509.Certificate, where, role string) error {
	if authority == nil {
		return fmt.Errorf("%w: %s certificate authority bundle holds no certificate", ErrIdentity, role)
	}
	if authority.KeyUsage != 0 && authority.KeyUsage&x509.KeyUsageCertSign == 0 {
		return fmt.Errorf("%w: %s in the %s CA bundle may not sign certificates", ErrIdentity, where, role)
	}
	return nil
}
