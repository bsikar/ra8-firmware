// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The outbound-assignment argument rule, stated once.
//
// protocol.Assignment is a semantic grant: a TaskRef of name and version, the
// catalog digest, the pinned source and a deadline. It carries no argv and no
// named values, deliberately, because an assignment that carried command text
// would be the concatenated command the whole catalog contract exists to keep
// away from a privileged boundary. The agent takes the name, looks the task up
// in its OWN embedded catalog, and runs it through executor.Run, which binds
// nothing.
//
// So an agent runs the reviewed steps and nothing else. That is correct for
// every task in the v1 catalog and for every task that declares no arguments,
// and it is silently wrong for a row that carries some: the plane bound argv
// at submission, wrote it to the row, and the runner would execute a shorter
// command than the row says it ran. A required positional at least fails on
// the agent, after the attempt is claimed, with an error about arguments
// rather than about the work. A flags-only schema does not even do that: the
// task runs with none of the requested flags and reports success against a row
// whose argv says otherwise.
//
// Refusing at the dispatcher is the narrow fix. The rule is a property of THIS
// carrier, not of the task: the same task with the same arguments is fine on
// the paths that do carry them, the local executor from the CLI (values bound
// in process) and a board HIL assignment (BoardHILAssignment.Args, re-checked
// by the board client on arrival). Refusing the schema at catalog admission
// instead would forbid those two as well, and refusing inside the agent would
// be telling it to judge an argv it was never sent.
func checkedAssignableArguments(raw []byte, definition catalog.Task) (persistedArguments, error) {
	persisted, err := checkedPersistedArguments(raw, definition)
	if err != nil {
		return persistedArguments{}, err
	}
	if len(persisted.Args) != 0 || len(persisted.Values) != 0 {
		return persistedArguments{}, fmt.Errorf(
			"task %q carries %d bound argument(s) and an outbound assignment carries none",
			definition.Name, len(persisted.Args))
	}
	return persisted, nil
}
