// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
	"os"
)

// errRetiredRecordIsNotAFile names the one thing this rule refuses: a receipt
// written against a name that is not the terminal record it retires.
var errRetiredRecordIsNotAFile = errors.New("sync receipt would retire a name that is not a terminal record")

// checkTheRetiredRecordIsAFileThisSpoolWrote holds MarkSynced to the file kind
// every other door in this package already asks about.
//
// A receipt is the only thing that retires evidence, and MarkSynced is the
// only thing that writes one. The condition it asked before writing was that
// the terminal record it names could be stat'ed, and os.Stat answers about
// what a name leads to rather than about the name: it follows a symlink to
// any file anywhere on the host, and it succeeds outright on a directory. So
// the one door in this package that can drop a record for good asked a weaker
// question than the doors that merely read one. readStarted refuses a start
// record that is not a regular file rather than following it, readRegularFile
// does the same for the terminal record and syncReceiptPresent for the
// receipt, all three by Lstat, and record_is_a_real_file.go states why: write
// is the only thing that creates a name under this directory, it writes a
// private temporary file and hard-links it into place, so a name here that is
// not a regular file was not written by this spool.
//
// What the weaker question costs is the loss this outbox exists to prevent,
// one step earlier than receiptRetiresRecord catches it. A directory or a
// symlink standing where <id>.finished.json belongs means the record is not
// there to upload, and a receipt written beside it says the server has it.
// Pending skips a record whose receipt sits beside it before reading anything
// else, so the missing evidence is never named by any later pass: the sweep
// reads clean and a run that happened on a disconnected host is gone. The
// ordinary cause is not an attack. A spool directory restored by a tool that
// rewrites regular files as links to a content store, or a half-finished
// manual recovery that left a directory behind under a name this package
// owns, both reach this door looking exactly like a record to os.Stat.
//
// Refused, it costs the one receipt: MarkSynced says which name is wrong,
// SyncPending reports the upload as unpersisted rather than synced, and the
// record stays pending behind a door that will open once the name under it is
// the file this spool wrote. A missing record is still reported as missing,
// by the Lstat error itself, because that is the same answer os.Stat gave and
// nothing about it was too weak.
func checkTheRetiredRecordIsAFileThisSpoolWrote(path string) error {
	info, err := os.Lstat(path)
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("%w: %q is not a regular file", errRetiredRecordIsNotAFile, path)
	}
	return nil
}
