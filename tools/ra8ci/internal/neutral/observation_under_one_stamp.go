// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"fmt"
	"time"
)

// observationUnderOneStamp holds a whole physical read to the freshness its
// single ObservedAt stamp claims for it, and returns the stamp that read can
// honestly carry.
//
// A receipt states one moment of observation, and both sides hold that one
// moment to maxObservationAge: the producer refuses an observation older than
// the budget (validObservation: now.Sub(o.ObservedAt) <= maxObservationAge,
// where that now is the stamp it writes as SignedAt) and the verifier refuses
// a signature further than the budget from it (checkObservationIsFresh).
// Nothing held the READ that produced the stamp to any span at all.
// ObserveNeutral read the clock once, after the last reading, so the stamp
// described the end of the read and said nothing about its beginning.
//
// The read is neither quick nor bounded. CheckIdle walks every entry under
// /proc, reading comm, cmdline and the whole fd directory of each live
// process, and fails closed on one it cannot inspect. Then every identity and
// state signal is read through its own adapter, and one of those adapters is
// the authenticated Tapo reader answering over the network; every sensor is
// read the same way. No reader here carries a deadline of its own: SignalReader
// takes a context and the only bound on any of them is whatever deadline the
// caller's ctx happens to hold, which ObserveNeutral neither sets nor requires.
//
// So board power could be read at T, a slow Tapo answer or a large procfs
// sweep could spend ninety seconds, the evidence could be marshalled at T+90s,
// and the stamp would say T+90s. The verifier then holds T+90s to a signature
// five seconds later, is satisfied, and releases the board on the strength of
// a power reading a minute and a half old. That is the whole of what the
// freshness rules exist to prevent, defeated by which end of the read got
// stamped, because the reading that decays is the EARLIEST one.
//
// THE RULE: bracket the read. A span longer than maxObservationAge cannot be
// sworn to under one instant whichever instant is chosen, so it is refused
// outright rather than stamped; inside the budget the stamp is the START of
// the window, the earliest moment any of its readings could be true, so the
// receipt understates its own freshness. Understating costs at most a refused
// release on a slow board agent, and the next challenge is seconds away.
// Overstating hands a board to another holder on a reading of a fixture that
// has since been touched.
//
// An inverted pair is refused rather than papered over: both stamps come from
// the same injected clock, so a finish before its own start is a clock nobody
// can defend, and a zero stamp on either side is no reading of a clock at all.
func observationUnderOneStamp(startedAt, finishedAt time.Time) (time.Time, error) {
	if startedAt.IsZero() || finishedAt.IsZero() {
		return time.Time{}, fmt.Errorf("%w: the physical read is not bracketed by two clock readings", ErrObservationAbsent)
	}
	if finishedAt.Before(startedAt) {
		return time.Time{}, fmt.Errorf("%w: the physical read finished before it began", ErrObservationAbsent)
	}
	if finishedAt.Sub(startedAt) > maxObservationAge {
		return time.Time{}, fmt.Errorf("%w: the physical read spanned %s, longer than the %s one stamp can carry",
			ErrObservationAbsent, finishedAt.Sub(startedAt), maxObservationAge)
	}
	return startedAt.UTC(), nil
}
