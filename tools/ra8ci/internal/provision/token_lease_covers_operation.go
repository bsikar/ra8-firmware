// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"errors"
	"fmt"
	"time"
)

// checkTokenLeaseCoversOperation holds the AppRole token a session is about to
// hand Terraform against the deadline that session's work actually runs under.
//
// LoginAppRole already bounds the lease Vault answers with (1s to 1h,
// approle.go) and OpenTerraformRuntime already bounds the per-command deadline
// (1s to 30m, terraform_cli.go). Both are sound on their own and nothing ever
// compared them. The lease was validated and then dropped on the floor: it was
// read off the login response, range-checked, and never stored, so by the time
// a token reached a session the plane no longer knew how long it was good for.
//
// The two bounds do not fit inside one another. A token good for a minute is
// within policy, a command allowed twenty minutes is within policy, and the
// pair is a credential handed to work that outlives it. VAULT_TOKEN is the
// session's ONLY Vault credential and the environment is deliberately built
// that way: OverlayEnvironment strips every inherited VAULT_ variable from the
// base environment and re-admits exactly this one, so a child that needs Vault
// has nothing else to fall back on when the lease ends mid-command.
//
// The failure that follows is the expensive kind. It does not arrive at this
// boundary with a reason; it arrives inside a running Terraform command, and
// run() reports every non-deadline failure as "Terraform command failed;
// reconcile state before retry" because it cannot tell what the child hit.
// An apply that dies that way leaves the ledger's apply intent consumed and
// the reservation's remote state half written, which is exactly the state
// Apply's own contract says it will not retry through.
//
// THE RULE IS ONE-SIDED. A lease LONGER than the deadline is the ordinary
// case (the usual configuration is an hour's lease against a twenty-minute
// command) and says nothing at all. Only a lease that cannot cover one
// command is refused. Do not make this two-sided: a long lease is not a
// finding, it is the shape every healthy deployment has.
//
// IT IS A FLOOR, NOT A GUARANTEE. WithSession hands the session to an opaque
// callback, so the plane cannot know how many commands that callback will run;
// the honest bound it CAN state is that the token must cover at least one, the
// same quantity run() holds each command to. A session that runs init, plan
// and apply back to back can still outlive a lease that cleared this check.
// Closing that would mean the callback declaring its own command budget, which
// is a contract change across every caller, not this check's business.
//
// NO MARGIN IS ADDED. The deadline is already the bound a command is held to,
// and a margin here would be a new number in a package whose other durations
// all come from an operator's configuration. The comparison is against the
// remaining lease at the moment the session opens, so time already spent since
// Vault answered counts: a token is judged by what is left of it, not by what
// it was issued with.
func checkTokenLeaseCoversOperation(token *AppRoleToken, operationTimeout time.Duration, now time.Time) error {
	if token == nil || token.lease <= 0 || token.issued.IsZero() {
		return errors.New("Vault token carries no lease this session can judge")
	}
	if operationTimeout <= 0 {
		return errors.New("Terraform operation timeout is unstated")
	}
	remaining := token.issued.Add(token.lease).Sub(now)
	if remaining < operationTimeout {
		return fmt.Errorf("Vault token has %s of its lease left and one Terraform command may run for %s",
			remaining.Round(time.Second), operationTimeout)
	}
	return nil
}
