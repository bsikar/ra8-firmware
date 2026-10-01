package board

// One lease, one measurement: the identity rule the estimate is computed on.
//
// YieldSample's own doc comment states it, "one lease leaves at most one yield
// sample, so a replayed commit updates a row instead of inventing a second
// handoff", and the store enforces it on the way in and refuses an
// identity-less row on the way out.
//
// EstimateHandoff never asked. A history carrying one lease's measurement
// twice is neither refused nor skipped: both copies land in the latency set,
// so a single handoff is weighted twice in a nearest-rank quantile computed
// over a deliberately small sample floor, and the Samples count reported
// beside the ETA says the estimate rests on more comparable handoffs than
// exist. Provenance then tells the requester so in as many words.
//
// The refusal is loud rather than a silent de-duplication, for the reason the
// store's own read refuses a row outside the cohort it asked for: two rows
// claiming one handoff mean the caller's history is wrong, and quietly keeping
// one of them hides which one was kept.
type measuredLeases map[string]struct{}

// admit records a lease about to contribute a latency, or refuses the second
// row claiming the same handoff.
//
// Only measured rows are held to it. A censored row carries no latency and
// moves no quantile: it is evidence that a yield was asked for and did not
// complete, and the estimator deliberately counts repeats of it (a board that
// fails to hand off five times running is five pieces of evidence, and the
// existing floor test pins that they never reach the sample floor). A row with
// no lease ID is left to the reader that produced it; the store refuses one
// already, and it can collide with nothing.
func (seen measuredLeases) admit(leaseID string) error {
	if leaseID == "" {
		return nil
	}
	if _, found := seen[leaseID]; found {
		return &Error{InvalidArgument, "yield history measures lease " + leaseID + " more than once"}
	}
	seen[leaseID] = struct{}{}
	return nil
}
