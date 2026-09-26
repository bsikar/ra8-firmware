// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import "strings"

// commLimit is how much of a process name /proc/<pid>/comm can carry. The
// kernel stores it in a TASK_COMM_LEN buffer of 16 bytes including the
// terminator, so a name longer than 15 bytes is written back short and there
// is no flag anywhere saying it was cut.
const commLimit = 15

// truncatedProtectedNames returns the comm-length prefix of every protected
// name the kernel cannot spell in full, lowercased for the same case-folded
// comparison the rest of the pass uses.
//
// Prefixes are taken from the raw bytes before lowercasing because the kernel
// cuts bytes, not characters. Every name that reaches here is ASCII: the
// built-in list is, and a profile-supplied name is held to profileNameRE
// ([a-zA-Z0-9][a-zA-Z0-9_.-]{0,63}), so cutting bytes and folding case cannot
// disagree about where the prefix ends.
func truncatedProtectedNames(names []string) map[string]bool {
	prefixes := make(map[string]bool)
	for _, name := range names {
		if len(name) > commLimit {
			prefixes[strings.ToLower(name[:commLimit])] = true
		}
	}
	return prefixes
}

// commNamesAProtectedProcess reports whether a process's comm names a
// protected tool, including one whose name is too long for comm to hold.
//
// CheckIdle compared comm to the protected set whole, and comm cannot hold a
// name longer than commLimit. Two entries of this package's own built-in list
// are longer: ra8-hil-privileged and ra8-hil-privileged.py both appear in comm
// as "ra8-hil-privile", which is in no protected set, so the comm arm read the
// privileged HIL helper as an ordinary process. A profile may add longer names
// still, and they are the names most likely to be long, since an operator
// writing a protected name writes the full script name.
//
// The cmdline arm is not a second chance for these. It reads
// /proc/<pid>/cmdline, which is empty for a zombie and for any process that
// rewrote its own argv, and an empty cmdline splits to one empty argument that
// matches nothing. So a long-named tool in either of those states cleared the
// pass on both arms, and a neutral receipt was signed over a board with the
// privileged helper still attached to it. That is the outcome this inspector
// exists to prevent, and it is not a failure to inspect, which CheckIdle
// already refuses: it is an inspection that read a truncated name as somebody
// else's.
//
// The prefix is only consulted for a comm of exactly commLimit bytes, which is
// the only length the kernel truncates to, so a shorter name is never
// prefix-matched against a longer protected one. A process genuinely named
// "ra8-hil-privile" is refused by this rule, and that is the deliberate
// direction: a name colliding with a protected tool's first 15 bytes is
// indistinguishable from the tool itself through comm, and this pass fails
// closed on what it cannot tell apart.
func commNamesAProtectedProcess(comm string, protected, truncated map[string]bool) bool {
	name := strings.ToLower(strings.TrimSpace(comm))
	if name == "" {
		return false
	}
	if protected[name] {
		return true
	}
	return len(name) == commLimit && truncated[name]
}
