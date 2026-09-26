// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"time"
)

// checkClientIdentityCoversOperation holds the state client identity this
// process is about to hand Terraform against the deadline one Terraform
// command actually runs under.
//
// checkTerraformStateClientIdentity, a few lines above the call site, asks
// whether the key pair is a currently valid client leaf: it is judged at
// time.Now() and it answers about this instant. mtls.ValidateClientIdentity
// refuses a leaf whose NotAfter has already passed and checkPresentedChain
// refuses an issuer whose NotAfter has already passed. Neither of them, and
// nothing else in this package, ever asked whether the certificate lasts as
// long as the work it is being issued for.
//
// The two bounds do not fit inside one another, exactly as the Vault lease and
// the command deadline did not. A certificate with a minute left is a
// currently valid client leaf, a command allowed twenty minutes is within
// policy, and the pair is an identity handed to work that outlives it. The
// state client is the session's ONLY credential for the state backend and the
// environment is deliberately built that way: HTTPBackendEnvironment writes
// TF_HTTP_CLIENT_CERTIFICATE_PEM once, for the whole session, and
// OverlayEnvironment admits nothing else that could stand in for it.
//
// The failure that follows is the expensive kind, and it is the same one the
// lease rule was written for. It does not arrive at this boundary with a
// reason; it arrives inside a running Terraform command as a refused
// handshake against the state server, and run() reports every non-deadline
// failure as "Terraform command failed; reconcile state before retry" because
// it cannot tell what the child hit. An apply that dies that way leaves the
// ledger's apply intent consumed and the reservation's remote state half
// written, which is exactly the state Apply's own contract says it will not
// retry through. Worse than the lease case: TF_HTTP_RETRY_MAX=1, so the
// backend gets one retry and then the state write is simply lost, while the
// infrastructure the apply already built stands.
//
// THE WHOLE PRESENTED CHAIN IS JUDGED, NOT THE LEAF. A key pair presents a
// chain and the far end walks every certificate in it, so the moment this
// identity stops being accepted is the EARLIEST NotAfter among them. An
// intermediate that outlives nothing is invisible to a rule that reads
// Certificate[0], and the presented chain is already what
// checkPresentedChain judges for the same reason.
//
// THE RULE IS ONE-SIDED. A certificate that outlasts the deadline is the
// ordinary case (the usual deployment rotates the state client daily against
// a twenty-minute command) and says nothing at all. Only an identity that
// cannot cover one command is refused. Do not make this two-sided: a
// long-lived certificate is not a finding, it is the shape every healthy
// deployment has.
//
// IT IS A FLOOR, NOT A GUARANTEE, for the same reason the lease rule is one:
// WithSession hands the session to an opaque callback, so the plane cannot
// know how many commands that callback will run. The honest bound it CAN
// state is that the identity must cover at least one, the same quantity run()
// holds each command to.
//
// AN UNSTATED DEADLINE IS NOT THIS RULE'S BUSINESS. The lease rule refuses
// one because checkTokenLeaseCoversOperation is only ever reached from
// WithSession, which always has the runtime's bounded timeout.
// HTTPBackendEnvironment is a package entry point in its own right and
// callers that only want the backend variables have no deadline to state, so
// an unstated one leaves the identity to the validity rule above rather than
// inventing a number here.
//
// NO MARGIN IS ADDED. The deadline is already the bound a command is held to,
// and a margin here would be a new number in a package whose other durations
// all come from an operator's configuration.
func checkClientIdentityCoversOperation(clientPair tls.Certificate, operationTimeout time.Duration, now time.Time) error {
	if operationTimeout <= 0 {
		return nil
	}
	if len(clientPair.Certificate) == 0 {
		return errors.New("Terraform client identity carries no certificate this deadline can be judged against")
	}
	var earliest time.Time
	var expiring *x509.Certificate
	for index, der := range clientPair.Certificate {
		certificate, err := x509.ParseCertificate(der)
		if err != nil {
			return fmt.Errorf("Terraform client identity carries an unreadable certificate at position %d: %w", index, err)
		}
		if expiring == nil || certificate.NotAfter.Before(earliest) {
			earliest = certificate.NotAfter
			expiring = certificate
		}
	}
	remaining := earliest.Sub(now)
	if remaining < operationTimeout {
		return fmt.Errorf("Terraform client identity (subject %q) has %s of its validity left and one Terraform command may run for %s",
			expiring.Subject.String(), remaining.Round(time.Second), operationTimeout)
	}
	return nil
}
