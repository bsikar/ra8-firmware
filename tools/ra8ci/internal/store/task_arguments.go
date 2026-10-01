// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The stored-arguments contract, stated once.
//
// A task row carries what it will run with: the argv the plane bound, the
// named values it bound them from, and the HIL contract the task was admitted
// under. The row is written once, at submission, and read back later by a
// plane that may hold a different catalog than the one that wrote it, so a
// reader may never take the row's word for any of the three.
//
// Re-deriving is what makes that safe, and catalog.ValidatePersistedArguments
// states the argument half of it: argv is required to equal a fresh binding of
// the stored values against the schema held NOW, so every element is tied back
// to a declared name and a catalog that changed underneath the row fails
// closed. This file adds the half that is about the row rather than the
// arguments: the board contract the row states has to be the one the reviewed
// definition states, in both directions. A row claiming board behaviour for a
// task whose definition declares none is as wrong as a HIL row that lost it.
type persistedArguments struct {
	Args   []string          `json:"argv"`
	Values map[string]string `json:"values"`
	HIL    *catalog.HILTask  `json:"hil"`
}

// checkedPersistedArguments decodes one task's stored arguments and requires
// them to agree with the reviewed definition held now, returning them only
// when they do.
//
// Unknown fields and trailing data are refused rather than ignored: a row
// carrying a key this reader does not know is a row written under a shape
// nobody here can judge, and running it would be running something other than
// what a reader of the current catalog can see.
func checkedPersistedArguments(raw []byte, definition catalog.Task) (persistedArguments, error) {
	var persisted persistedArguments
	if len(bytes.TrimSpace(raw)) == 0 {
		return persistedArguments{}, fmt.Errorf("task %q has no stored arguments", definition.Name)
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&persisted); err != nil {
		return persistedArguments{}, fmt.Errorf("task %q stored unreadable arguments: %v", definition.Name, err)
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		return persistedArguments{}, fmt.Errorf("task %q stored trailing argument data", definition.Name)
	}
	if err := checkPersistedBoardContract(persisted.HIL, definition); err != nil {
		return persistedArguments{}, err
	}
	if err := definition.ValidatePersistedArguments(persisted.Values, persisted.Args); err != nil {
		return persistedArguments{}, err
	}
	return persisted, nil
}

// checkPersistedBoardContract holds the row's HIL contract to the reviewed
// one. Equality is the rule, not presence: HILTask is all scalars, so a
// reviewed field that moved (a manifest path, a mode, a timeout) makes the
// stored contract a different one, and the row is refused instead of driving
// a board session under terms review no longer states.
func checkPersistedBoardContract(stored *catalog.HILTask, definition catalog.Task) error {
	switch {
	case definition.HIL == nil && stored == nil:
		return nil
	case definition.HIL == nil:
		return fmt.Errorf("task %q stored a board contract its reviewed definition does not declare", definition.Name)
	case stored == nil:
		return fmt.Errorf("task %q stored no board contract, its reviewed definition declares one", definition.Name)
	case *stored != *definition.HIL:
		return fmt.Errorf("task %q stored a board contract that is not its reviewed one", definition.Name)
	}
	return nil
}
