// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"fmt"
	"time"
)

// The three timeouts are one budget, not three independent numbers.
//
// New range-checks each of them on its own: a request may take up to thirty
// seconds, an operation up to thirty minutes, a task poll up to thirty
// seconds. Nothing held them against each other, and they are not independent.
// operationTimeout is the deadline on opCtx, the context every request an
// operation makes is issued under, so it is the ceiling the other two spend
// against rather than a fourth thing that happens in parallel.
//
// Two shapes pass the individual bounds and cannot work:
//
// An operation budget below the per-request ceiling. Clone, Start, Stop and
// Destroy all open opCtx first and then issue several requests under it, so a
// budget that cannot cover even one request at its own stated ceiling has no
// arrangement of requests that fits.
//
// A poll interval above the operation budget. waitTask issues the mutation,
// reads the task once, and if the task is still running sleeps pollInterval
// before reading again. A sleep longer than the whole budget means the second
// reading is never taken: the select wakes on ctx.Done every time.
//
// What that costs is specific, and it is why this is worth refusing at
// construction. Both shapes fail INSIDE a mutation, after the request has
// gone out, where mutateAndVerify has no way to tell a deadline from a lost
// answer and reports an UnknownOutcomeError. That error is documented never to
// authorize a retry, so every stop, start, clone and destroy this client is
// asked for ends as an operation an operator has to reconcile by hand, for a
// configuration that is wrong in a way nothing in the running system names.
//
// The rules are one-sided on purpose. An operation budget far LARGER than a
// request ceiling is the ordinary shape (a fifteen-second request ceiling
// under a five-minute operation), and a poll interval far SHORTER than the
// budget is the ordinary shape too. Only the inversions are refused.

// checkTimeoutsFitTogether holds the three configured durations to each other
// after each has passed its own range check. It takes the resolved values, the
// ones a Client is actually built with, so a default filled in by New is
// judged exactly as an operator-supplied value is.
func checkTimeoutsFitTogether(requestTimeout, operationTimeout, pollInterval time.Duration) error {
	if operationTimeout < requestTimeout {
		return fmt.Errorf("%w: operation timeout %s is below the per-request timeout %s, so no operation can complete a single request",
			ErrInvalid, operationTimeout, requestTimeout)
	}
	if pollInterval > operationTimeout {
		return fmt.Errorf("%w: task poll interval %s is above the operation timeout %s, so a running task is never read a second time",
			ErrInvalid, pollInterval, operationTimeout)
	}
	return nil
}
