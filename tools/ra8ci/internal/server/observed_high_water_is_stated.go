package server

// An agent's generation observation must actually state a high-water.
//
// POST /v1/boards/{id}/agent/observe is the one board door that passed its
// body straight to the state machine. Every other agent-facing door on this
// surface refuses a zero first: the ack door will not take generation 0 or
// installed_generation 0, the checkpoint and segment doors will not take a
// zero generation, and the reason doors will not take an empty why. The
// observe door took whatever decoded.
//
// That matters here more than anywhere else on the surface, because of what
// the state machine does with the number. observeGeneration quarantines the
// board when the reported high-water is below the one already recorded
// (board.go: "agent high-water contradicts database"), and a quarantine is
// the most expensive verdict the board has: the lease is gone, the queue
// stops, and the board only comes back through an operator-run recovery that
// ends on a neutral receipt. A board that has ever granted a lease has a
// non-zero AgentHighWater, so zero is below it, so zero quarantines it.
//
// And zero is exactly the value this wire cannot distinguish from silence.
// The body is decoded with unknown fields disallowed, which catches a
// misspelled field but not a missing one: a request that omits high_water
// entirely, or sends it as null, decodes to the same 0 as one that means it.
// A client with a half-built request, or a field lost to a refactor, could
// take a working board out of service without ever stating a number. No
// other door on this surface lets an unwritten field decide anything, and
// this is the door where an unwritten field decides the most.
//
// Refusing zero costs the plane nothing it can otherwise say. Against a board
// whose recorded high-water is already 0 an observation of 0 is a no-op: not
// above the generation, not below the recorded mark, no event, no change. So
// the only request this turns away is one whose sole possible effect was to
// quarantine a board on a number nobody wrote. A genuine divergence still has
// its voice: an agent that installed a generation the database does not have
// reports it as the number it is, above the mark rather than below it, and is
// quarantined exactly as before. This holds the report to being a report.
func observedHighWaterIsStated(highWater uint64) bool {
	return highWater != 0
}
