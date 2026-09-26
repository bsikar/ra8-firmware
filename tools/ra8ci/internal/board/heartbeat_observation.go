package board

import "time"

// The grant is itself an observation of the holder.
//
// ObserveHolderLiveness says so in as many words: LastSeenAt is "the most
// recent beat, or the grant itself when no beat has arrived yet: being granted
// the board is itself an observation that the holder was there". So the
// question holderHeartbeat has to answer is not whether a beat is newer than
// the last beat, it is whether the beat is newer than anything already known
// about this holder, and until the first beat arrives that is the grant.
//
// The out-of-order arm asked only half of it, comparing the beat with
// LastHeartbeatAt, which is zero until the first beat lands. A beat stamped
// before the grant therefore passed: a report replayed from the previous
// holder of the same board, or one stamped by a runner whose clock sits behind
// the server's. What it left behind is a lease Validate refuses outright, with
// "retained lease carries a heartbeat from before its grant", and Apply
// validates the snapshot it is handed, so every later command against that
// board fails on a lease the reducer wrote itself. A holder that reports too
// early wedges its own board.
//
// A beat at the grant instant is not recorded either, for the same reason a
// beat at LastHeartbeatAt is not: it is not a later observation than the one
// already held. Validate accepts such a stamp from elsewhere, so nothing here
// contradicts it; this decides only what the reducer writes.
func beatAddsAnObservation(lease *Lease, now time.Time) bool {
	return now.After(lease.LastHeartbeatAt) && now.After(lease.GrantedAt)
}
