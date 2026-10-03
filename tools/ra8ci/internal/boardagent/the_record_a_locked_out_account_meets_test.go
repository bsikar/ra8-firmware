// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

// The two ways the service account can be locked out of its own state, and
// what the store has to do about each.
//
// Every other refusal in this store is decided by reading the record. These
// two are decided by the account's standing on the filesystem, which the
// checks before them cannot see: a mode the stat call is happy with and the
// open still refuses, and a directory the store may read but may not write.
// Both have to end as ErrUnsafeState, because a board agent that treats
// either as "no record yet" would acknowledge a grant it never durably
// recorded.
