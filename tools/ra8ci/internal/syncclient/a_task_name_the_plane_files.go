// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"
	"strings"
	"unicode/utf8"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// maxFilableTaskNameBytes is local_runs.task_name's bound, restated rather
// than imported: internal/store does not depend on this package and this
// package does not depend on it for the rule, only for the receipt type.
const maxFilableTaskNameBytes = 128

// ErrUnfilableTaskName is the refusal of a record naming a task the plane
// has no column to file it under.
var ErrUnfilableTaskName = errors.New("local record names a task the plane will not file")

// entry.Task is the one identity on a spooled record that names WHAT was
// run, and it is the identity every other door leans on: the result's own
// word for the task is held against it (checkUploadedResultNamesItsTask),
// the terminal record is held against the start record by it
// (spool.checkFinishMatchesStart), and the server looks the reviewed
// definition up BY it (server.offlineInput). The sweep never looked at it.
//
// It is frozen before the first command runs, so nothing after the freeze
// can improve it, and it comes back off disk through json.Unmarshal, which
// fills whatever the file holds. spool.BeginWithMetadata now bounds its
// length and encoding at the freeze (#1952), but a record written by an
// older client, or a file edited between the freeze and the sweep, is read
// by Pending exactly as written.
//
// THE RULE, and it is the store's, not a new one: 1..128 bytes,
// TrimSpace(name) == name, and text a text column can hold
// (store.validateLocalRun, local_sync.go:201-202; namesATextColumnCanHold
// refuses invalid UTF-8, C0 controls, DEL and C1). A name is not free text.
// It is printed beside every local run in history, matched byte for byte
// against the catalog's reviewed definitions, and read back by an operator
// deciding what a run was. A leading space makes two entries out of one
// task in anything that groups by name, and an escape sequence rewrites the
// terminal that prints the history.
//
// DELIBERATELY NOT the comparison the server makes: offlineInput resolves
// the name in that server's catalog and refuses one it does not hold. That
// needs the catalog, so a client guessing at it would refuse evidence the
// plane would have taken. Same cut as the catalog digest door (#1956) and
// the classification door (#1957): shape here, membership there.
//
// *** HONESTY: the store refuses every shape below, so nothing ill-formed
// was reaching the column. What the refusal buys is where and how the sweep
// stops. Refused there, the client reads "upload local <id> returned HTTP
// 400" with no field named, indistinguishable from a server that is merely
// unwell, and that error ends the sweep, so every unsynced record behind it
// in the outbox waits on this pass and on every pass after it. Refused
// here, the operator is told which record and which field.
func checkUploadedTaskNameIsOneThePlaneFiles(entry spool.Entry) error {
	name := entry.Task
	switch {
	case name == "":
		return fmt.Errorf("%w: no task name", ErrUnfilableTaskName)
	case len(name) > maxFilableTaskNameBytes:
		return fmt.Errorf("%w: task name is %d bytes, the plane files %d",
			ErrUnfilableTaskName, len(name), maxFilableTaskNameBytes)
	case strings.TrimSpace(name) != name:
		return fmt.Errorf("%w: task name %q is padded with space", ErrUnfilableTaskName, name)
	case !taskNameThePlaneCanFile(name):
		return fmt.Errorf("%w: task name carries text the plane cannot file", ErrUnfilableTaskName)
	}
	return nil
}

// taskNameThePlaneCanFile mirrors store.namesATextColumnCanHold: valid
// UTF-8 with no C0 control, no DEL and no C1 control. Text outside ASCII is
// not the thing being refused.
func taskNameThePlaneCanFile(value string) bool {
	if !utf8.ValidString(value) {
		return false
	}
	for _, char := range value {
		if char < 0x20 || char == 0x7f || (char >= 0x80 && char <= 0x9f) {
			return false
		}
	}
	return true
}
