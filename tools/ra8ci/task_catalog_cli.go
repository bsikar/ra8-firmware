// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The reviewed catalog is the contract a task runs under: its scope, its
// deadline, and the exact argv of every step. Its digest is what the plane
// records on an attempt, a HIL claim and a local spool entry, and it is how a
// person reconciles a row against the binary in front of them.
//
// `ra8ci tasks` printed names and nothing else, so neither the contract nor
// the digest could be read from the binary that carries them. Reading the
// manifest out of the source tree is not the same answer: the tree is what
// review changed, the binary is what ran.
const tasksUsage = "usage: ra8ci tasks [--digest|--json]"

// errTasksUsage marks a refusal the caller should report as misuse rather than
// as a failure to read the catalog. The two exit differently.
var errTasksUsage = errors.New(tasksUsage)

// catalogReport is the whole reviewed catalog as one document. The digest is
// carried beside the definitions deliberately: a definition quoted without the
// digest it came from cannot be tied to the attempt row that names one.
type catalogReport struct {
	SchemaVersion int            `json:"schema_version"`
	Digest        string         `json:"digest"`
	Tasks         []catalog.Task `json:"tasks"`
}

// tasksCommand writes the embedded catalog in the form the caller asked for.
// It validates its arguments before writing anything, so a refusal never
// leaves a half-written document on the stream.
func tasksCommand(out io.Writer, definitions *catalog.Catalog, args []string) error {
	if len(args) > 1 {
		return fmt.Errorf("tasks takes at most one option: %w", errTasksUsage)
	}
	option := ""
	if len(args) == 1 {
		switch args[0] {
		case "--digest", "--json":
			option = args[0]
		default:
			return fmt.Errorf("unknown tasks option %q: %w", args[0], errTasksUsage)
		}
	}
	if definitions == nil {
		return errors.New("no task catalog")
	}
	switch option {
	case "--digest":
		_, err := fmt.Fprintln(out, definitions.Digest())
		return err
	case "--json":
		return writeCatalogReport(out, definitions)
	default:
		for _, name := range definitions.Names() {
			if _, err := fmt.Fprintln(out, name); err != nil {
				return err
			}
		}
		return nil
	}
}

// writeCatalogReport emits one compact JSON document, the shape every other
// ra8ci verb emits. Tasks keep manifest order, the order review reads them in.
func writeCatalogReport(out io.Writer, definitions *catalog.Catalog) error {
	names := definitions.Names()
	report := catalogReport{
		SchemaVersion: catalog.SchemaVersion,
		Digest:        definitions.Digest(),
		Tasks:         make([]catalog.Task, 0, len(names)),
	}
	for _, name := range names {
		task, found := definitions.Task(name)
		if !found {
			return fmt.Errorf("task %q named by the catalog is missing from it", name)
		}
		report.Tasks = append(report.Tasks, task)
	}
	return json.NewEncoder(out).Encode(report)
}
