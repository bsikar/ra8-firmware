// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/tls"
	"crypto/x509"
	"fmt"
)

// checkChainDepthTheIssuersAllow holds a presented chain to the depth its own
// authorities allow.
//
// checkPresentedChain judges each link on its own properties and
// checkPresentedChainIsAPath judges that the links form a path, each one
// certifying the one before it. Between them they read everything about a
// chain except the one thing an authority says about the certificates BELOW
// it: pathLenConstraint, the basic-constraints field that states how many
// certificate authorities may sit between this one and an end-entity
// certificate. Nothing here ever read it, so a chain whose links are all live,
// all allowed to sign and all correctly signed by the link above is presented
// on every handshake even when the topmost of them forbids the very shape
// being sent.
//
// It is the ordinary result of a CA rotation done one layer at a time. A root
// that issued a single intermediate under pathLenConstraint 0, correct while
// that intermediate signed leaves directly, forbids the chain the moment a
// second intermediate is slipped underneath it, which is exactly what issuing
// a new signing tier under the existing root looks like. The operator
// assembles leaf, new intermediate, old intermediate, and every certificate in
// the file is impeccable.
//
// The far end is where it fails, and it fails as the same opaque denial the
// rest of this package exists to name first. Go's path builder holds every
// candidate parent to its own constraint: a certificate with basic
// constraints and a pathLenConstraint refuses a path carrying more
// intermediates below it than the field allows, which arrives at the operator
// wrapped as "certificate signed by unknown authority" from a host whose leaf,
// window, usages and signatures are all correct.
//
// The reading mirrors the verifier this tree links against exactly, including
// how it treats the field's absence: a parsed certificate carries -1 when the
// extension states no constraint, and any value from zero up is a stated
// limit. The count compared against it is the number of certificates the chain
// sends between this link and the leaf, which is this link's position minus
// one, the same arithmetic the verifier does on the chain it has built so far.
//
// It runs after checkPresentedChainIsAPath rather than beside it: a chain that
// is not a path at all has no depth worth stating, and the operator should
// read the broken link first.
//
// The message names the position, the limit the link declares and the number
// of authorities actually sent below it, because the fix is to remove a tier
// from the file or to have the constraint reissued, and those two numbers are
// what tells an operator which. It names the subject and the public
// fingerprint, never the key, the same way every other refusal here does.
func checkChainDepthTheIssuersAllow(identity tls.Certificate, role string) error {
	for position := 1; position < len(identity.Certificate); position++ {
		issuer, err := x509.ParseCertificate(identity.Certificate[position])
		if err != nil {
			return fmt.Errorf("%w: parse the certificate at position %d of the presented %s chain: %v",
				ErrIdentity, position, role, err)
		}
		if !issuer.BasicConstraintsValid || issuer.MaxPathLen < 0 {
			// No stated constraint: the link says nothing about what may
			// sit below it, and this rule has nothing to hold it to.
			continue
		}
		sent := position - 1
		if sent <= issuer.MaxPathLen {
			continue
		}
		where := fmt.Sprintf("subject %q sha256 %s", issuer.Subject.String(), Fingerprint(issuer))
		return fmt.Errorf("%w: %s at position %d of the presented %s chain allows %d certificate authorities below it, and the chain sends %d",
			ErrIdentity, where, position, role, issuer.MaxPathLen, sent)
	}
	return nil
}
