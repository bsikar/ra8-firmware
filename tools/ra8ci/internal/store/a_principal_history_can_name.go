// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

// principalNamesOneReadableActor reports whether the principal a local run is
// filed under is an identity this store can write and an operator can read
// back.
//
// validateLocalRun bounded it by LENGTH alone, 1..256 bytes, which is what
// local_runs.principal_id states too (0005_offline_sync.sql). Every other
// identity on this input now answers namesATextColumnCanHold: the task name
// and the step keys since #1930, the repository and branch since #1932. The
// principal was the one left, and it is the identity the other two are looked
// up BY: local_runs_principal_time_idx is (principal_id, received_at DESC),
// the uniqueness that makes a retry idempotent is (principal_id, local_id),
// and LookupLocalRunReceipt matches on it byte for byte.
//
// The claim here is narrower than the one the task name answers, and worth
// stating plainly. On the live path the principal is not attacker-supplied:
// offlineSubmit takes it from AuthorizeCertificate, which reads
// api_principals.principal_id out of Postgres, so it is already valid UTF-8
// with no NUL by the time this door sees it. What that column does NOT state
// is any character rule at all (0001_initial.sql: text NOT NULL UNIQUE), so a
// principal seeded with a newline, an escape sequence, or a C1 control is
// stored, authorized, and propagated into every local run it submits, into
// the index above and into the audit rows beside it. A principal carrying a
// newline reads as two actors in anything line-oriented, and one carrying an
// escape sequence rewrites the terminal that prints the history.
//
// So this states the store's own contract at the store's own door rather than
// inheriting it from whoever built the input, the same reason
// ValidateArtifactSet stopped trusting its caller to hand it one attempt.
// LocalRunInput is exported and validateLocalRun is what stands behind it.
func principalNamesOneReadableActor(principal string) bool {
	return namesATextColumnCanHold(principal)
}
