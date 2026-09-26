// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"errors"
	"fmt"
)

// errOpenDemandOverflows names the one thing this rule refuses: a store that
// answered ListOpen with more demand than the pass asked for.
var errOpenDemandOverflows = errors.New("open demand list is longer than the batch the pass asked for")

// checkOpenDemandFitsTheBatch reads what one pass was actually shown.
//
// Pass reads open demand once, with a limit, and then reports what it did:
// Scanned, Waiting, Advanced, Unchanged, Concluded, Failed. Every counter
// names a decision, and the set of them reads as an account of the open
// demand. It is an account of at most BatchSize of it. The store's own query
// is ORDER BY queued_at, demand_key LIMIT $1, so a pass whose limit is full
// saw the oldest window of the queue and nothing behind it, and said nothing
// about that.
//
// The window does not necessarily move. Demand inside the grace period is
// left alone, and demand the forge agrees is unchanged is counted and not
// written, so neither moves a row out of `phase <> 'completed'` and neither
// changes its position in that ordering. With more open demand than one batch
// holds, the same oldest rows come back every pass and the demand behind them
// is never asked about at all. That is exactly the demand this package exists
// for: a completion delivery that never arrived leaves a unit open forever
// unless something asks, and the reconciler is the thing that asks.
//
// Reporting the cap is the honest half of that and the half a pass can do on
// its own. ListOpen takes a limit and no cursor, so this rule does not invent
// paging the contract has no room for; it states that the pass read a capped
// view, which is what an operator needs to raise the batch or the cadence.
//
// A list LONGER than the limit is a different thing and is refused. The pass
// bounds its own work by that number, and every unit in the list costs a
// request to the forge, so a store answering with more than it was asked for
// spends API budget the pass never agreed to spend. It is also a store this
// pass can believe nothing else from: ListOpen's limit is the only thing
// holding the pass to a bounded amount of work.
func checkOpenDemandFitsTheBatch(open []Event, batchSize int) (truncated bool, err error) {
	if len(open) > batchSize {
		return false, fmt.Errorf("%w: %d units for a batch of %d",
			errOpenDemandOverflows, len(open), batchSize)
	}
	return len(open) == batchSize, nil
}
