// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"crypto/tls"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
)

// checkTerraformStateClientIdentity judges the client key pair this process is
// about to hand Terraform by the plane's one client-identity rule,
// mtls.ValidateClientIdentity, instead of by a local copy of part of it.
//
// The backend environment carries the state client's certificate and private
// key to a Terraform child process, so this is the moment the plane decides
// that what it is about to present is a usable client identity at all. The
// package already defers to mtls for the other half of the same handshake:
// the server CA bundle goes through mtls.ServerAuthorities a few lines below,
// for exactly the reason stated there, that a bundle which parses and then
// authenticates nobody reaches the operator as an unexplained denial.
//
// The leaf half was a copy. It checked three of the five things
// ValidateClientIdentity checks, in the same terms, and the two it left out
// are the two that fail at the handshake rather than here:
//
//   - KEY USAGE. TLS 1.3 client authentication is a signature made with this
//     key. A leaf that declares key usages without digital signature cannot
//     produce one, so Terraform reaches the state server, fails the
//     handshake, and the apply dies mid-operation with a transport error
//     rather than at the boundary with a reason.
//   - THE PRESENTED CHAIN. A key pair presents a chain, and every issuer in
//     it is walked by the far end exactly as the leaf is judged here. An
//     expired intermediate, or one that may not sign certificates, is invisible
//     to a rule that only ever looks at Certificate[0].
//
// Two copies of one rule only ever agree until one of them is edited, and the
// copy here was already the weaker of the two. The refusal keeps its own
// wording so the caller still says which certificate is meant, and wraps
// mtls.ErrIdentity so a caller can classify a local identity problem without
// matching on message text.
//
// This does not make the server's decision. What a certificate is allowed to
// do on the state surface is store.AuthorizeCertificate's call, taken against
// api_principals with the terraform_state grant; this decides only that the
// identity is presentable.
func checkTerraformStateClientIdentity(clientPair tls.Certificate, now time.Time) error {
	if len(clientPair.Certificate) == 0 || clientPair.PrivateKey == nil {
		return errors.New("Terraform client certificate and private key do not match")
	}
	if err := mtls.ValidateClientIdentity(clientPair, now); err != nil {
		return fmt.Errorf("Terraform client certificate is not a currently valid client leaf: %w", err)
	}
	return nil
}
