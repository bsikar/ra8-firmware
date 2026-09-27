// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// maxUploadedEnvelope is the longest wall-clock span the control plane's
// offline door will read a record for. It is the server's own bound, not a
// second opinion: server.checkLocalEnvelopeIsStated refuses a record whose
// FinishedAt.Sub(StartedAt) is longer than 25 hours, which is the ceiling
// catalog.ValidateTask holds every reviewed deadline to plus an hour of slack
// for the spool write and for clock skew between the two stamps. The two
// numbers are measurements of the same pair of stamps.
const maxUploadedEnvelope = 25 * time.Hour

// ErrUnreadableEnvelope is the refusal of a local record whose own stamps the
// offline door will not read.
var ErrUnreadableEnvelope = errors.New("local record states an envelope the offline door does not read")

// A terminal record's two stamps are the only thing durable history has to say
// how long the attempt took, and the spool writes both from the local host's
// clock without reading them back. spool.Pending already refuses the shape a
// stepped clock produces most often, a finish before the start, and it holds
// the finish stamp of the terminal record equal to the start record's. What it
// does not ask is whether the stamps describe a span at all.
//
// Three shapes survive every door this client already holds a record to. A
// zero StartedAt passes the order rule trivially, because any real finish is
// after the zero time. A zero FinishedAt passes it beside a zero start, and
// Pending only asks that the finish be present, not that it be a time. And a
// span of days or years passes it outright: a start record written before a
// host went offline, or a clock that steps FORWARD between Begin and Finish,
// puts the two stamps as far apart as the correction, and the record is in
// order the whole way.
//
// What the zero costs is stated on the server's own rule: time.Time.Sub
// saturates, so FinishedAt.Sub(zero) is exactly math.MaxInt64 nanoseconds, and
// that is the duration a run would carry in durable history for an attempt the
// record never claimed to have started.
//
// *** HONESTY: the server refuses all three below, so nothing ill-formed was
// reaching the database. What the refusal buys is the same thing the
// unstated-identity and record-size doors buy either side of it: where the
// sweep stops and what it says when it does. Refused there, the client reads
// back "upload local <id> returned HTTP 400", which is also what a server that
// is merely unwell says, and that error ends the whole sweep. Pending hands a
// record back until a synced marker sits beside it, so the same record is read
// again, posted again and refused again on every pass, and every unsynced
// record behind it in the outbox waits behind it forever. Refused here, the
// operator is told which record and which stamp, before the bytes leave the
// host that wrote them.
//
// The bound is judged on the record alone and never against the clock now.
// This host's clock is the one under suspicion, and a rule that read time.Now
// could refuse a record the server would accept, which is the one direction a
// client-side door must not fail in.
func checkUploadedEnvelopeIsReadable(entry spool.Entry) error {
	if entry.FinishedAt == nil || entry.FinishedAt.IsZero() {
		return fmt.Errorf("%w: no finish stamp", ErrUnreadableEnvelope)
	}
	if entry.StartedAt.IsZero() {
		return fmt.Errorf("%w: no start stamp, so the run's duration is unbounded", ErrUnreadableEnvelope)
	}
	if span := entry.FinishedAt.Sub(entry.StartedAt); span > maxUploadedEnvelope {
		return fmt.Errorf("%w: the envelope spans %s, over the %s the server reads",
			ErrUnreadableEnvelope, span, maxUploadedEnvelope)
	}
	return nil
}
