// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// A beat comes back carrying the stamp the holder is supposed to act on.
// boardclient.HolderLiveness documents NextBeatBy as "the only number a
// holder can act on: report before it or start reading as overdue to anyone
// watching", and KeepAlive owns the only wait that decides whether it is met.
// Nothing read it. The loop slept liveness.Interval and nothing else, so the
// stamp arrived, was stored in a struct field, and was never consulted again.
//
// Interval alone is not the same answer, for two reasons that compound.
//
// The server measures silence from the beat it recorded, not from the reply
// the holder received, so NextBeatBy is already partly spent by the time it
// lands here. Sleeping a full interval from the reply therefore posts every
// beat one round trip late, every cycle. On its own that is survivable:
// board.HolderLiveness is only Overdue past HeartbeatGraceBeats, three whole
// intervals of silence.
//
// The blip is what spends the grace. When ReportAlive fails with anything
// that is not settled, the loop keeps the interval it last knew and waits
// that long again, which is right, because being unseen is not being
// finished. But it waits blind: the previous NextBeatBy is now nearer or
// already behind, and the loop cannot tell a stamp two seconds out from one
// two minutes out. Two consecutive blips at the server's own cadence is
// three intervals of silence, exactly the grace, and the loop has not tried
// any harder for it. The first beat of a loop is the worst shape of this,
// because the interval it starts from is defaultHeartbeatInterval, one whole
// minute, a number the server never said: a holder on a two-second cadence
// whose first beat blips sits out thirty beats before its second attempt.
//
// What it costs is what beatHalvesAgree was written for. Overdue is a report
// and never ends a lease, so nothing here loses a board. It loses the truth
// about one: an operator reading a quiet board decides whether a holder has
// crashed, and a holder that is working normally and simply reporting late
// reads exactly like one that is gone. That is the reading the whole beat
// exists to provide, and a wait that ignores the due stamp is the one thing
// that can make it wrong while the holder is perfectly healthy.

// nextBeatWait is how long to wait before reporting again: the server's
// cadence, shortened so the next beat lands before the report the server
// said it was expecting.
//
// A zero stamp is a server that named no due time, and the cadence stands
// alone. A stamp already passed does not mean beat now and keep beating: the
// floor is minHeartbeatInterval, the same one beatInterval applies, because
// a holder may not report at itself faster than it may observe the board.
func nextBeatWait(interval time.Duration, nextBeatBy, now time.Time) time.Duration {
	if nextBeatBy.IsZero() {
		return interval
	}
	remaining := nextBeatBy.Sub(now)
	if remaining >= interval {
		return interval
	}
	if remaining < minHeartbeatInterval {
		return minHeartbeatInterval
	}
	return remaining
}

// dueStamp carries the due time forward across a refused beat. A beat that
// answered replaces the stamp, including replacing a stamp with none when the
// server stops naming one. A beat that did not answer changes nothing, which
// is what keeps a blip waiting against the last due time this holder was
// actually given rather than against a bare interval.
func dueStamp(previous time.Time, reported boardclient.HolderLiveness, answered bool) time.Time {
	if !answered {
		return previous
	}
	return reported.NextBeatBy
}
