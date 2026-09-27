// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// checkHostFactsNameOneHost holds a receipt's two host snapshots to the one
// machine they are both readings of.
//
// The pair exists to be compared. The agent reads the runner once before the
// task and once after it, and an operator reads the two side by side to say
// what the machine looked like while the work ran: free memory before and
// after, load before and after. Every one of those comparisons assumes the two
// readings came off the same host, and nothing checked it. HostFacts.Validate
// judges a snapshot on its own terms, and checkHostFactsBracketTheAttempt
// compares only the two capture stamps, so a receipt could state a linux amd64
// runner at the start and a windows arm64 one at the end and be admitted.
//
// Both readings are taken in one agent process from runtime.GOOS and
// runtime.GOARCH (agent/host_linux.go and agent/host_windows.go), which are
// fixed for the life of that binary, so an honest pair cannot disagree about
// either. A pair that does is not a machine that changed; it is two machines,
// or one field a guest filled in freely, and every later comparison of the
// resource numbers beside them is then a comparison between hosts.
//
// The plane cannot catch it afterwards either. It checks the OS an agent
// states against the agent record at acknowledgment and nowhere else
// (store/dispatch.go, AcknowledgeAgentAssignment), so the receipt's own pair is
// never held to that record or to itself, and the end snapshot is what lands as
// the attempt's durable resource sample.
//
// Only the two fields that cannot change are judged. Cores and memory are
// measured rather than compiled in, and a runner can legitimately be resized
// between readings, so holding those equal would refuse an honest receipt. Load
// and free memory are expected to differ; that difference is the whole point of
// taking two readings.
func checkHostFactsNameOneHost(receipt TerminalReceipt) error {
	start, end := receipt.HostFactsAtStart, receipt.HostFactsAtEnd
	if start.OS != end.OS {
		return fmt.Errorf("%w: the host snapshots name %s at the start and %s at the end",
			ErrInvalid, start.OS, end.OS)
	}
	if start.Arch != end.Arch {
		return fmt.Errorf("%w: the host snapshots name %s at the start and %s at the end",
			ErrInvalid, start.Arch, end.Arch)
	}
	return nil
}
