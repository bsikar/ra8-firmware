// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
)

// checkServerTrustCoversOperation holds the state server CA bundle this
// process is about to hand Terraform against the deadline one Terraform
// command actually runs under.
//
// It is the third side of a question this package has already answered twice.
// checkTokenLeaseCoversOperation asks whether the Vault token outlives one
// command; checkClientIdentityCoversOperation asks whether the client
// identity does. The handshake those two serve has a third credential in it,
// written into the same environment by the same function, and nothing asked
// the same question about it. mtls.ServerAuthorities is called at time.Now()
// a few lines above, so the bundle was judged for this instant and never for
// the work it is being issued for.
//
// The two bounds do not fit inside one another, exactly as they did not for
// the lease and the leaf. A bundle whose last live authority has a minute
// left authenticates the state server today, a command allowed twenty minutes
// is within policy, and the pair is a trust file handed to work that outlives
// it. TF_HTTP_CLIENT_CA_CERTIFICATE_PEM is written once for the whole session
// and OverlayEnvironment admits nothing that could stand in for it, so the
// child has no other way to trust the state server.
//
// The failure that follows is the expensive kind, and it is the one the other
// two rules were written for. It does not arrive at this boundary with a
// reason; it arrives inside a running Terraform command as "certificate
// signed by unknown authority" against the state server, and run() reports
// every non-deadline failure as "Terraform command failed; reconcile state
// before retry" because it cannot tell what the child hit. An apply that dies
// that way leaves the ledger's apply intent consumed and the reservation's
// remote state half written. TF_HTTP_RETRY_MAX=1, so the backend gets one
// retry and then the state write is simply lost while the infrastructure the
// apply already built stands.
//
// THE BUNDLE IS JUDGED AS A SET, NOT CERTIFICATE BY CERTIFICATE. A CA bundle
// is a set of authorities and any live one of them can verify the server, so
// the earliest expiry in the file says nothing: a rotation deliberately puts
// the outgoing authority beside the incoming one and mtls.ServerAuthorities
// accepts that for exactly this reason. What is refused is a bundle that will
// authenticate NOBODY by the time the command ends, which is the same rule
// mtls already states, asked at a later instant.
//
// SO THE RULE IS NOT COPIED, IT IS RE-ASKED. checkTerraformStateClientIdentity
// says in its own words why a second copy of an mtls rule is worth avoiding:
// two copies only ever agree until one of them is edited. This calls the same
// function at now plus the deadline, so whatever mtls decides a usable bundle
// is, this decides about the end of the command.
//
// THE RULE IS ONE-SIDED. An authority that outlasts the deadline is the
// ordinary case and says nothing at all. Only a bundle that cannot cover one
// command is refused.
//
// IT IS A FLOOR, NOT A GUARANTEE, for the same reason the other two are:
// WithSession hands the session to an opaque callback and the plane cannot
// know how many commands that callback will run. The honest bound it CAN
// state is that the trust must cover at least one, the same quantity run()
// holds each command to.
//
// AN UNSTATED DEADLINE IS NOT THIS RULE'S BUSINESS. HTTPBackendEnvironment is
// a package entry point in its own right and a caller that only wants the
// backend variables has no deadline to state, so an unstated one leaves the
// bundle to the validity check above rather than inventing a number here.
func checkServerTrustCoversOperation(bundle []byte, operationTimeout time.Duration, now time.Time) error {
	if operationTimeout <= 0 {
		return nil
	}
	if _, err := mtls.ServerAuthorities(bundle, now.Add(operationTimeout)); err != nil {
		return fmt.Errorf("Terraform server CA bundle cannot authenticate the state server for the %s one Terraform command may run: %w",
			operationTimeout, err)
	}
	return nil
}
