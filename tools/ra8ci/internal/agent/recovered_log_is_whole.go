// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

// recoveredLogIsWhole answers whether the plane holds every byte this attempt
// produced, now that the one kept-back chunk has been accepted at the last
// offer. droppedBytes is what the uploader refused or never offered AFTER that
// chunk.
//
// The uploader keeps exactly one chunk back. When a chunk exhausts its bounded
// retries the error becomes sticky, that chunk is held, and every later write
// is turned away at the door without being offered to the plane. flushGrace
// then offers the held chunk once more on its own window, and a plane that was
// only briefly unreachable accepts it.
//
// That recovery was invisible to the receipt. flushGrace moved the final log
// sequence forward and dropped the held chunk, but left the sticky error in
// place, so uploader.status() still reported a failure and terminalReceipt
// turned a passing attempt into outcome "failed" with ErrorCode
// log_upload_error and EvidenceComplete cleared. The plane held the whole log
// and was told the evidence was broken, which is the opposite of what the last
// offer exists for: a verdict of failed on a green task is read by everything
// downstream as the task failing.
//
// Clearing the error unconditionally would be the other lie. The sticky error
// is also a gate: while it stands, every byte the run writes afterwards is
// refused and lost, and those bytes are gone whatever the held chunk does.
// When any were refused the log really is incomplete past the held chunk, and
// the receipt must keep saying so even though the last offer landed. So the
// error is cleared only when the held chunk was the end of the log and nothing
// behind it was ever turned away.
func recoveredLogIsWhole(droppedBytes int64) bool {
	return droppedBytes == 0
}

// acceptHeldChunk records the plane accepting the kept-back chunk: the final
// sequence moves to it, nothing is held any more, and the sticky error is
// lifted when, and only when, the log is whole again.
func (uploader *logUploader) acceptHeldChunk() {
	if uploader.pending == nil {
		return
	}
	uploader.sequence = uploader.pending.Sequence
	uploader.pending = nil
	if recoveredLogIsWhole(uploader.dropped) {
		uploader.err = nil
	}
}
