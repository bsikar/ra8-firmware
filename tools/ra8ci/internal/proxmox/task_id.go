// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"fmt"
	"strconv"
	"strings"
)

// A Proxmox task ID is a colon-delimited record, not an opaque string:
//
//	UPID:node:pid:pstart:starttime:type:id:user:comment
//
// It is the only durable handle this package has on a mutation it issued, it
// is persisted by the caller as the proof that the mutation happened, and it
// is replayed into a task-status URL path on reconciliation. Reading its
// fields is therefore the cheapest check available: a task ID that names
// another node, another operation kind, or another guest can be refused here,
// before a request is sent, rather than after a round trip that an unavailable
// API would never complete.
const (
	taskIDFieldCount = 9
	taskIDNodeField  = 1
	taskIDTypeField  = 5
	taskIDGuestField = 6
)

// taskID is the parsed form. Only the fields this package judges are kept;
// the pid, start stamps, user, and comment are carried by the original string.
type taskID struct {
	Node  string
	Type  string
	Guest string
}

// taskTypeFor names the Proxmox worker type a lifecycle kind runs as. An
// unknown kind returns "", which parseTaskID refuses rather than waves through.
func taskTypeFor(kind string) string {
	switch kind {
	case "clone":
		return "qmclone"
	case "start":
		return "qmstart"
	case "stop":
		return "qmstop"
	case "destroy":
		return "qmdestroy"
	default:
		return ""
	}
}

// parseTaskID reads a task ID and holds it to the operation it claims to be.
// The guest field is parsed for shape but deliberately not compared with the
// reservation: waitTask compares the authoritative id the API reports, and
// which guest a clone task is filed under is a Proxmox convention this package
// cannot settle away from a real cluster.
func parseTaskID(upid, node, kind string) (taskID, error) {
	wantType := taskTypeFor(kind)
	if wantType == "" {
		return taskID{}, fmt.Errorf("%w: unknown operation kind %q", ErrInvalid, kind)
	}
	if !upidPattern.MatchString(upid) {
		return taskID{}, fmt.Errorf("%w: malformed Proxmox task ID", ErrInvalid)
	}
	fields := strings.SplitN(upid, ":", taskIDFieldCount)
	if len(fields) != taskIDFieldCount {
		return taskID{}, fmt.Errorf("%w: Proxmox task ID is not a complete record", ErrInvalid)
	}
	for index, field := range fields[:taskIDFieldCount-1] {
		if field == "" {
			return taskID{}, fmt.Errorf("%w: Proxmox task ID field %d is empty", ErrInvalid, index)
		}
	}
	parsed := taskID{
		Node:  fields[taskIDNodeField],
		Type:  fields[taskIDTypeField],
		Guest: fields[taskIDGuestField],
	}
	if parsed.Node != node {
		return taskID{}, fmt.Errorf("%w: Proxmox task ID names node %q, not the configured node", ErrInvalid, parsed.Node)
	}
	if parsed.Type != wantType {
		return taskID{}, fmt.Errorf("%w: Proxmox task ID is a %s task, not %s", ErrInvalid, parsed.Type, wantType)
	}
	guest, err := strconv.Atoi(parsed.Guest)
	if err != nil || guest < 100 || guest > 999999999 {
		return taskID{}, fmt.Errorf("%w: Proxmox task ID names no guest", ErrInvalid)
	}
	return parsed, nil
}
