// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"unicode/utf8"
)

// checkTheManifestPathIsOneARecordCanHold holds the other keyed text field a
// reviewed HIL contract states to text a record can carry.
//
// It sits beside the board-model rule deliberately. Those two strings are the
// pair hil_observations conflicts on together, (attempt_id, manifest_path,
// board_model, fixture_revision, profile_sha256, program_family, mode), and
// the same row is read back by (manifest_path, board_model, ...) as the
// cohort a handoff estimate is drawn from. The model half was held to text a
// record can carry; this half was not.
//
// validHILManifestPath judges the SHAPE of the path and judges it well: an
// examples/ prefix, a /hil.conf suffix, no backslash, no parent traversal,
// already clean, at most 512 bytes. Every one of those is about where the
// path points. None of them is about the bytes in between, so a path carrying
// a NUL, a newline, an escape sequence or an invalid UTF-8 byte was admitted
// whole: path.Clean does not strip a control character, and a control
// character is neither a separator nor a dot.
//
// The cost is the board-model rule's cost, for the same reason. A Postgres
// text column holds no NUL byte and no invalid UTF-8 whatever its length, and
// the migration's own CHECK (manifest_path LIKE 'examples/%' AND
// length(manifest_path) <= 512) asks nothing about either. So those two
// spellings were not refused where refusing them is free; they reached the
// INSERT after the board had already been granted, flashed and observed, and
// reported work that genuinely happened as an unavailable store rather than
// as the invalid definition it came from.
//
// The rest of the control range is the reading half, the store's own
// reasoning for the names it files runs under (namesATextColumnCanHold in
// text_a_text_column_can_hold.go), and it bites harder on a path than on a
// model: this is the name an operator greps a board's history by and the name
// a person types to open the manifest themselves. A path carrying a newline
// reads as two paths in anything line-oriented, and one carrying an escape
// sequence rewrites the terminal that prints it.
//
// This is an admission rule and not part of ValidateTask's runtime re-check,
// for the reason ValidateTask states: a runtime holding this task was granted
// it against a reviewed digest, so refusing it there would retroactively
// refuse work review already admitted.
func checkTheManifestPathIsOneARecordCanHold(task Task) error {
	if task.HIL == nil {
		return nil
	}
	if !manifestPathARecordCanHold(task.HIL.ManifestPath) {
		return fmt.Errorf("%w: task %q declares a manifest path no record could hold: %q",
			ErrInvalidCatalog, task.Name, task.HIL.ManifestPath)
	}
	return nil
}

// manifestPathARecordCanHold reports whether a reviewed manifest path is text
// this store can write and a reader can read back.
//
// It deliberately states no alphabet and no length. Both already belong to
// validHILManifestPath, which owns the question of where the path points; this
// one only asks whether the bytes it is spelled with are bytes a record can
// carry. Anything printable outside ASCII stays admitted, because a manifest
// lives in a repository tree whose names this package has no standing to
// narrow.
func manifestPathARecordCanHold(manifest string) bool {
	if !utf8.ValidString(manifest) {
		return false
	}
	for _, char := range manifest {
		if char < 0x20 || char == 0x7f || (char >= 0x80 && char <= 0x9f) {
			return false
		}
	}
	return true
}
