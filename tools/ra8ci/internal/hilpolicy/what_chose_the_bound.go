// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import "math"

// Decision exists for audit: it carries the chosen bound and the evidence
// behind it so a reader can work out later why the bench waited as long as it
// did. Two things could overrule that evidence without leaving a mark.
//
// The FLOOR. Choose raises its answer to the declared fallback, so an app
// declaring HIL_TIMEOUT_S=600 over observations that settle around 30s gets
// 600. The record still said source "observed" beside a mean, a maximum and a
// standard deviation that are all an order of magnitude smaller, which is
// arithmetic nobody can reproduce: every number in the audit record is
// truthful and none of them chose the bound.
//
// The CEILING, and this is the one that costs a run. Choose clamps at
// MaximumSeconds, so evidence demanding 4900s comes back as 3600 with source
// "observed" and a maximum observed of 3500. The bound is now BELOW what the
// observations asked for, the observe step is cut off mid-run, and it is
// reported timed out: the exact failure declared_present.go and
// declared_spelling.go both exist to keep a config from causing. That censored
// observation then feeds the next decision, the 1.5x headroom pushes the
// demand higher still, and every later run clamps to the same 3600 while the
// record goes on describing evidence that never chose anything.
//
// So the decision states what chose it. EvidenceSeconds is what the
// observations alone demanded before either bound was applied, and the two
// flags say which one overruled them. Source is left exactly as it was: it
// names where the numbers came from, and the existing readers of it
// (hilspec.Decide, and the catalog rule that holds an observation timeout
// inside its deadline) are entitled to the answer they already have.
//
// Only the observed path sets any of this. When Choose falls back for want of
// MinimumSamples, Source already says "hil.conf" or "default" and there is no
// evidence to be overruled.
//
// The two flags cannot both be true. A declared fallback is bounded by
// MaximumSeconds before it reaches here, so a bound raised to the floor is at
// or under the ceiling by construction; the test sweep pins that rather than
// this comment claiming it.
func boundFromEvidence(evidence float64, fallback int) (seconds int, raisedToFallback, clippedToMaximum bool) {
	bound := evidence
	if float64(fallback) > bound {
		bound, raisedToFallback = float64(fallback), true
	}
	if bound > MaximumSeconds {
		bound, clippedToMaximum = MaximumSeconds, true
	}
	return int(math.Ceil(bound)), raisedToFallback, clippedToMaximum
}
