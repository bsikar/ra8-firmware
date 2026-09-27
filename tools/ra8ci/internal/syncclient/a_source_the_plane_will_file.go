// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// maxFilableSourceBytes is the bound local_runs.repository and
// local_runs.branch carry, restated rather than imported: internal/store
// does not depend on this package and this package depends on it only for
// the receipt type.
const maxFilableSourceBytes = 512

// ErrUnfilableSource is the refusal of a record naming a source the plane
// has no column to file it under.
var ErrUnfilableSource = errors.New("local record names a source the plane will not file")

// The identity door beside this one (checkUploadedSourceIdentityIsStated)
// asks whether a record CLAIMS a source at all: a repository that is not
// empty, a 40-hex commit, a verification word, and a snapshot digest
// present or absent to match it. It never asks whether the two free-text
// halves of that claim, the repository and the branch, are text the plane
// can file. Nothing else client-side does either: spool.BeginWithMetadata
// bounds them at the freeze (identities_the_plane_will_file.go), and that
// is the freeze, not this door, because Pending reads the record back off
// disk through json.Unmarshal, which fills whatever the file holds.
//
// THE RULE, and it is the store's, not a new one
// (store.validateLocalRun, local_sync.go:194-195): the repository is
// 1..512 bytes, the branch is at most 512 bytes and may be empty, and both
// are text a text column can hold (valid UTF-8, no C0 control, no DEL, no
// C1). The empty branch is deliberate on the store's side and kept here: a
// detached checkout names a commit and no branch, and that is an honest
// account of where a run happened rather than a half-formed one.
//
// These two strings are the provenance an operator reads when they ask
// where a run came from, printed beside the commit in durable history. An
// escape sequence in either rewrites the terminal that prints it, and a
// repository longer than the column simply does not arrive.
//
// DELIBERATELY NOT a claim about the SHAPE of either one: no host, no
// owner/name split, no ref-name grammar. The plane files whatever names
// the checkout it read (catalog.VerifyCheckout reports the remote and the
// branch as git states them), so a client inventing a grammar here would
// refuse evidence the plane would have taken. Same cut as the catalog
// digest door (#1956), the classification door (#1957) and the task name
// door (#1960): shape the column can hold here, meaning there.
//
// *** HONESTY: the store refuses every shape below, so nothing ill-formed
// was reaching the column. What the refusal buys is where and how the
// sweep stops. Refused there, the client reads back "upload local <id>
// returned HTTP 400" with no field named, which is also what a server that
// is merely unwell says, and that error ends the whole sweep; Pending
// re-offers the record until a synced marker sits beside it, so the same
// record is posted and refused on every pass and every unsynced record
// behind it waits forever. Refused here, the operator is told which record
// and which field, before the bytes leave the host that wrote them.
func checkUploadedSourceNamesOneThePlaneFiles(entry spool.Entry) error {
	source := entry.Source
	if len(source.Repository) > maxFilableSourceBytes {
		return fmt.Errorf("%w: repository is %d bytes, the plane files %d",
			ErrUnfilableSource, len(source.Repository), maxFilableSourceBytes)
	}
	if len(source.Branch) > maxFilableSourceBytes {
		return fmt.Errorf("%w: branch is %d bytes, the plane files %d",
			ErrUnfilableSource, len(source.Branch), maxFilableSourceBytes)
	}
	for _, named := range []struct {
		field string
		value string
	}{{"repository", source.Repository}, {"branch", source.Branch}} {
		if !taskNameThePlaneCanFile(named.value) {
			return fmt.Errorf("%w: %s carries text the plane cannot file", ErrUnfilableSource, named.field)
		}
	}
	return nil
}
