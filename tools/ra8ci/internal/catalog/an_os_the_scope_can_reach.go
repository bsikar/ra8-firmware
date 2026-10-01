// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// hostOSAScopePins states, for the scopes that pin one, the host OS every
// agent of that class runs. It is read out of the dispatch rule the server
// applies to an authenticated agent (store.agentForCertificate): a runner or a
// linux-vm agent whose declared OS is not linux is denied, and so is a
// windows-vm agent whose declared OS is not windows. Those two refusals are
// the whole mapping; restating them here is what lets a catalog rule see it.
//
// The other three scopes pin nothing and are deliberately absent. Both
// safe-local scopes run wherever the agent already is, and the claim filter
// picks tasks by the host's own OS (store.ClaimAgentTask asks
// definition.SupportsOS(facts.OS)), so either OS is a real answer there. A hil
// task reaches a board through the board commons rather than a host class, and
// the catalog states nothing that would settle which OS drives it.
var hostOSAScopePins = map[string]string{
	"runner":     "linux",
	"linux-vm":   "linux",
	"windows-vm": "windows",
}

// HostOSAScopePins returns the host OS a scope pins, if it pins one.
func HostOSAScopePins(scope string) (string, bool) {
	goos, pinned := hostOSAScopePins[scope]
	return goos, pinned
}

// checkTheDeclaredOSReachesTheScopesHosts refuses a reviewed task whose scope
// sends it to a class of host whose OS the task never declares.
//
// ValidateTask judges the two fields apart and never together. It holds the
// scope to the six the plane knows and the OS list to linux, windows, and no
// duplicate, so scope "windows-vm" with os ["linux"] satisfies both rules
// exactly, and every dispatch door in this package judges argv rather than
// either field. The definition is admitted.
//
// It can then never run. The server hands a windows-vm assignment only to an
// agent whose declared OS is windows, because any other is denied at
// authentication; the agent that receives it re-checks the definition against
// its own host before it acks, and the executor re-checks again before it
// spawns anything, both by asking task.SupportsOS(runtime.GOOS). A task
// declaring only linux fails that check on the one host class its scope can
// reach, so the work is refused as ErrUnsupportedOS at the far end of a
// dispatch that had nowhere else to go.
//
// That is what makes it worth a door rather than a runtime refusal: the
// failure surfaces on a host, at claim time, as an unsupported-OS error
// against a definition review already blessed, and the reader has to hold the
// host-class mapping in their head to see that the two fields contradict each
// other. Refused at admission, the contradiction is stated where both fields
// are written down.
//
// A task declaring BOTH is admitted under every scope: it names the OS its
// scope reaches and one more, which is what a definition meant for two host
// classes looks like. Only a task that never names the OS its own scope pins
// is refused.
//
// This is an admission rule, applied where a manifest is read, not a re-check
// of a task already persisted against a reviewed digest: a definition admitted
// under an older rule keeps running, the same line ValidateTask draws against
// the dispatch seam.
func checkTheDeclaredOSReachesTheScopesHosts(task Task) error {
	pinned, pins := HostOSAScopePins(task.Scope)
	if !pins {
		return nil
	}
	for _, declared := range task.OS {
		if declared == pinned {
			return nil
		}
	}
	return fmt.Errorf("%w: task %q is scoped %q, which only ever reaches a %s host, and declares os %s; the agent re-checks its own OS against that list before it runs a step, so this dispatch has nowhere it can land",
		ErrInvalidCatalog, task.Name, task.Scope, pinned, strings.Join(task.OS, ", "))
}
