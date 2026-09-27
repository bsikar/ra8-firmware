// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"fmt"
	"strings"
)

// artifactPathSet accumulates the paths one attempt's manifests name and
// refuses the entry that cannot exist beside an entry already in the set.
//
// ValidateArtifactSet held the set to one entry per path, comparing the
// strings exactly. That is the store's identity for an artifact, which is
// (attempt_id, path), and it is the right rule for a row. It is not the whole
// rule for a COLLECTION: these paths are laid out under one directory when
// the evidence is written back out, and two paths that differ as strings can
// still be one name on the host doing the writing. The set passed, the rows
// were filed, and the collision surfaced later at extraction, where the
// artifact that lost is simply missing and nothing says which one it was.
//
// Two ways a pair collides, both of them refused here.
//
// A file where another entry needs a directory. build/ra8.elf.map and build
// are different strings and different rows, but one of them asks for build to
// be a directory and the other asks for it to be a plain file. Whichever is
// written second fails or replaces the first, depending on the host. This is
// the collision a collector can produce honestly: a task declares an output
// directory's contents and the directory itself in one step's outputs.
//
// A name that differs only in case. logs/Run.txt and logs/run.txt are two
// files on the Linux box that produced them and one file on a Windows or
// macOS host that writes them out, where the second write lands on top of the
// first. The folding is the same argument reservedDeviceNames already makes a
// few lines above in this package: an artifact is produced in a guest and
// written back out on a host that may run either OS, so the stricter rule is
// the shared one. Refusing the set at the door says which two entries
// disagree, while allowing it loses one of them silently much later.
//
// The folding is ASCII-only, which is all it needs to be: ValidArtifactPath
// already refuses every byte outside 0x21..0x7e, so a path in a valid set has
// no case beyond A-Z and strings.ToLower cannot fold two of them together by
// some locale rule.
type artifactPathSet struct {
	files map[string]string
	dirs  map[string]string
}

func newArtifactPathSet(size int) *artifactPathSet {
	return &artifactPathSet{
		files: make(map[string]string, size),
		dirs:  make(map[string]string, size),
	}
}

// add files one more path, or reports the entry already in the set that the
// host could not write beside it.
func (set *artifactPathSet) add(path string) error {
	folded := strings.ToLower(path)
	if earlier, taken := set.files[folded]; taken {
		if earlier == path {
			return fmt.Errorf("%w: artifact set names %q twice", ErrInvalid, path)
		}
		return fmt.Errorf("%w: artifact set names %q and %q, one file on a host that folds case", ErrInvalid, earlier, path)
	}
	if needed, taken := set.dirs[folded]; taken {
		return fmt.Errorf("%w: artifact set needs %q as both a file and a directory, for %q", ErrInvalid, path, needed)
	}
	for _, parent := range foldedParents(folded) {
		if earlier, taken := set.files[parent]; taken {
			return fmt.Errorf("%w: artifact set needs %q as both a file and a directory, for %q", ErrInvalid, earlier, path)
		}
		set.dirs[parent] = path
	}
	set.files[folded] = path
	return nil
}

// foldedParents lists the directories a path needs, outermost first. The path
// itself is not one of them.
func foldedParents(folded string) []string {
	var parents []string
	for index := strings.Index(folded, "/"); index >= 0; {
		parents = append(parents, folded[:index])
		next := strings.Index(folded[index+1:], "/")
		if next < 0 {
			break
		}
		index += 1 + next
	}
	return parents
}
