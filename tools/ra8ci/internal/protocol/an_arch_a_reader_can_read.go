// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

// archIsReportable reports whether a host's stated architecture is text the
// plane can file and a reader can read back. It is the one host fact nothing
// bounded. HostFacts.Validate holds every other field of a guest-origin
// measurement to something: cores and memory to a range, free memory to the
// total beside it, load to a real non-negative number, the capture stamp to
// non-zero and, on a receipt, to the side of the attempt it was taken on
// (receipt_host_facts_window.go), the OS to two names and the load kind to
// whichever of them was stated. Arch was asked only to be non-empty, so every
// byte after the first was accepted: a control character, an escape sequence, a
// newline, a NUL, bytes that are not valid UTF-8 at all, or a megabyte of text
// up to the decoder's own MaxJSONBytes ceiling.
//
// These facts are guest-origin by design, which is the reason the rest of them
// are bounded here rather than trusted: the type says so, and the server never
// authenticates them, it only checks that the OS matches the agent record it
// already holds (store/dispatch.go, AcknowledgeAgentAssignment refusing an ack
// whose OS differs). Nothing anywhere makes that check for the architecture.
// The store marshals the whole struct to JSON and files it as the attempt's
// resource sample, and the migrations bound no field inside it, so an operator
// reading what the runner looked like is reading whatever the guest wrote.
//
// The agent's own readers can only ever state Go's name for the machine they
// are running on (agent/host_linux.go and agent/host_windows.go, both
// runtime.GOARCH), and every name Go reports is lowercase ASCII letters and
// digits: the longest is eleven bytes, and one of them, 386, is digits alone,
// so the rule cannot ask for a leading letter. That is the whole alphabet an
// honest reading uses, and holding the field to it refuses the shapes that
// break the reading of it: a name carrying a newline reads as two facts in
// anything line-oriented, a name carrying an escape sequence rewrites the
// terminal that prints it, and a name that is not valid UTF-8 is refused by the
// text column it reaches rather than at the boundary where a malformed message
// belongs.
//
// The bound is deliberately wider than any name in use rather than a list of
// the names Go has today: a new port is a new string, and refusing it here
// would make this rule the reason a runner cannot report itself. What it
// refuses is text that is not an architecture name at all.
func archIsReportable(arch string) bool {
	if arch == "" || len(arch) > maxArchBytes {
		return false
	}
	for _, char := range arch {
		if (char < 'a' || char > 'z') && (char < '0' || char > '9') {
			return false
		}
	}
	return true
}

// maxArchBytes is well above every name Go reports (mips64p32le, eleven bytes)
// so that a future port is not refused for its length alone.
const maxArchBytes = 32
