// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

// taskNameNamesAReviewedTask reports whether a grant's task name is a name a
// reviewed task could actually carry. It is the last free-text field left in an
// assignment. Every other identity in the grant states its own shape:
// ValidID holds both UUIDs to canonical lowercase UUIDv7, ValidSHA256 holds the
// catalog digest and the snapshot digest to a lowercase full-length hex digest,
// ValidCommit holds the commit to a Git object ID, the versions and the fencing
// token must be positive, the deadline non-zero and the remaining time inside
// MaxDeadlineMS. The task name was asked only to be non-empty.
//
// The far end declares the alphabet. A task and every step it owns are admitted
// into the catalog only through validName (catalog.go, called from
// ValidateTask), which accepts lowercase letters, digits and hyphens and
// nothing else, so no reviewed task in existence can be named any other way.
// The names in the tree today are far inside it: the longest is twelve bytes.
//
// What the gap costs is the refusal, not the execution. A grant naming a task
// that cannot exist still fails, but it fails late and says the wrong thing:
// the agent carries the name to its catalog lookup and reports that the task is
// not an embedded read-only non-board definition (agent.go, execute), which is
// a statement about a real task's scope, tier and OS support. For a name
// carrying a newline, an escape sequence or a megabyte of text, that diagnosis
// is simply false, and it is the one an operator reads while the actual problem
// is that the message was malformed. A grant is the one message here the runner
// takes instruction from, so a malformed one belongs refused at the boundary
// with the other malformed messages.
//
// The bound is stated here rather than borrowed from the catalog: this package
// is the protocol boundary and must judge a message without the catalog loaded,
// and the length ceiling is the half validName does not state at all.
func taskNameNamesAReviewedTask(name string) bool {
	if name == "" || len(name) > maxTaskNameBytes {
		return false
	}
	for _, char := range name {
		if (char < 'a' || char > 'z') && (char < '0' || char > '9') && char != '-' {
			return false
		}
	}
	return true
}

// maxTaskNameBytes is well above every task the catalog declares today, the
// longest of which is twelve bytes, so a new reviewed task is not refused for
// its length alone.
const maxTaskNameBytes = 64
