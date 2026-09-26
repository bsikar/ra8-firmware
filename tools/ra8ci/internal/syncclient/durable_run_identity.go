// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Two of the three fields on a receipt are ours before the upload starts: the
// local ID and the payload digest are held against what this client sent, so a
// server cannot move them. LocalRunID is the one field the server alone
// decides, and it was held only to shape (store.ValidID). The durable mapping
// is one local record to one server run, so the same LocalRunID coming back for
// two different local records inside one sweep is not a shape the server can
// honestly produce: either the lookup keyed on something other than the local
// ID, or the reply was not the reply to this request. Writing both synced
// markers anyway retires two distinct records against one durable run, and the
// evidence that they were ever separate attempts is the marker itself.
//
// This is a sweep-scoped rule, not a spool-wide one: markers written by an
// earlier sweep are not re-read, so it refuses only what this sync observed.
// A replayed upload is unaffected. The server answers a replay from its own
// local_id mapping, so distinct local records still take distinct run IDs.
func checkDurableRunIsUnclaimed(claimed map[string]string, localID string, receipt store.LocalRunReceipt) error {
	if claimed == nil {
		return nil
	}
	if earlier, found := claimed[receipt.LocalRunID]; found {
		return fmt.Errorf("durable run %s was already receipted for local %s", receipt.LocalRunID, earlier)
	}
	claimed[receipt.LocalRunID] = localID
	return nil
}
