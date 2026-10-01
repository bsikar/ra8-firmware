// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func sourced(repository, branch string) spool.Entry {
	return spool.Entry{
		SchemaVersion: uploadableSchemaVersion,
		ID:            "0123456789abcdef0123456789abcdef",
		Task:          "unit-tests",
		Source: spool.SourceIdentity{
			Repository:   repository,
			Branch:       branch,
			CommitSHA:    strings.Repeat("a", 40),
			Verification: "unverified",
		},
	}
}

func TestASourceTheCheckoutNamedIsUploaded(t *testing.T) {
	if err := checkUploadedSourceNamesOneThePlaneFiles(
		sourced("git@github.com:bsikar/ra8-firmware.git", "ra8ci/dev")); err != nil {
		t.Fatalf("an ordinary source was refused: %v", err)
	}
}

func TestADetachedCheckoutNamingNoBranchIsUploaded(t *testing.T) {
	if err := checkUploadedSourceNamesOneThePlaneFiles(
		sourced("git@github.com:bsikar/ra8-firmware.git", "")); err != nil {
		t.Fatalf("a detached checkout was refused: %v", err)
	}
}

func TestARepositoryLongerThanTheColumnIsRefused(t *testing.T) {
	err := checkUploadedSourceNamesOneThePlaneFiles(
		sourced(strings.Repeat("r", maxFilableSourceBytes+1), "ra8ci/dev"))
	if !errors.Is(err, ErrUnfilableSource) {
		t.Fatalf("an oversized repository was accepted: %v", err)
	}
}

func TestABranchLongerThanTheColumnIsRefused(t *testing.T) {
	err := checkUploadedSourceNamesOneThePlaneFiles(
		sourced("ra8-firmware", strings.Repeat("b", maxFilableSourceBytes+1)))
	if !errors.Is(err, ErrUnfilableSource) {
		t.Fatalf("an oversized branch was accepted: %v", err)
	}
}

func TestTheSourceBoundIsBytesNotRunes(t *testing.T) {
	// 256 two-byte runes are 512 bytes, the column's whole width, and one
	// more rune is over it even though the string is 257 characters long.
	if err := checkUploadedSourceNamesOneThePlaneFiles(
		sourced(strings.Repeat("é", maxFilableSourceBytes/2), "ra8ci/dev")); err != nil {
		t.Fatalf("a source exactly the column's width was refused: %v", err)
	}
	err := checkUploadedSourceNamesOneThePlaneFiles(
		sourced(strings.Repeat("é", maxFilableSourceBytes/2+1), "ra8ci/dev"))
	if !errors.Is(err, ErrUnfilableSource) {
		t.Fatalf("a source one rune over the column was accepted: %v", err)
	}
}

func TestAnEscapeSequenceInTheRepositoryIsRefused(t *testing.T) {
	err := checkUploadedSourceNamesOneThePlaneFiles(sourced("ra8\x1b[2Jfirmware", "ra8ci/dev"))
	if !errors.Is(err, ErrUnfilableSource) {
		t.Fatalf("a repository carrying an escape sequence was accepted: %v", err)
	}
}

func TestANewlineInTheBranchIsRefused(t *testing.T) {
	err := checkUploadedSourceNamesOneThePlaneFiles(sourced("ra8-firmware", "ra8ci/dev\nnot-a-branch"))
	if !errors.Is(err, ErrUnfilableSource) {
		t.Fatalf("a branch carrying a newline was accepted: %v", err)
	}
}

func TestInvalidUTF8InTheSourceIsRefused(t *testing.T) {
	err := checkUploadedSourceNamesOneThePlaneFiles(sourced("ra8-\xff-firmware", "ra8ci/dev"))
	if !errors.Is(err, ErrUnfilableSource) {
		t.Fatalf("a repository carrying invalid UTF-8 was accepted: %v", err)
	}
}

func TestTextOutsideASCIIIsStillUploaded(t *testing.T) {
	// The rule refuses control characters, not languages: a checkout whose
	// path carries non-ASCII text is an ordinary checkout.
	if err := checkUploadedSourceNamesOneThePlaneFiles(
		sourced("/home/bsikar/dépôts/ra8-firmware", "ra8ci/dév")); err != nil {
		t.Fatalf("a non-ASCII source was refused: %v", err)
	}
}

func TestTheSourceDoorDoesNotChooseARepository(t *testing.T) {
	// Deliberately no grammar: no host, no owner/name split, no ref-name
	// rules. Whatever git reported about the checkout is what the plane
	// files, so this door only asks what the column can hold.
	for _, repository := range []string{"..", "ra8 firmware", "https://example.invalid/x", "HEAD"} {
		if err := checkUploadedSourceNamesOneThePlaneFiles(sourced(repository, "HEAD")); err != nil {
			t.Fatalf("the door judged the repository %q: %v", repository, err)
		}
	}
}

func TestAnEmptyRepositoryIsLeftToTheIdentityDoor(t *testing.T) {
	// checkUploadedSourceIdentityIsStated is the door that refuses a record
	// claiming no repository at all, and it names the field when it does.
	// Restating it here would report the same record under two errors.
	if err := checkUploadedSourceNamesOneThePlaneFiles(sourced("", "ra8ci/dev")); err != nil {
		t.Fatalf("the empty repository was refused here rather than at the identity door: %v", err)
	}
	if err := checkUploadedSourceIdentityIsStated(sourced("", "ra8ci/dev")); !errors.Is(err, ErrUnstatedSourceIdentity) {
		t.Fatalf("the identity door did not refuse the empty repository: %v", err)
	}
}
