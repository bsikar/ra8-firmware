// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

const (
	aCommit   = "0123456789abcdef0123456789abcdef01234567"
	aSnapshot = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
)

func verifiedEntry() spool.Entry {
	return spool.Entry{
		SchemaVersion: 2,
		ID:            "00112233445566778899aabbccddeeff",
		Task:          "clang-format",
		Source: spool.SourceIdentity{
			Repository:     "bsikar/ra8-firmware",
			Branch:         "dev",
			CommitSHA:      aCommit,
			SnapshotSHA256: aSnapshot,
			Verification:   "verified",
		},
	}
}

func unverifiedEntry() spool.Entry {
	entry := verifiedEntry()
	entry.Source.Verification = "unverified"
	entry.Source.SnapshotSHA256 = ""
	return entry
}

func TestAStatedIdentityIsAccepted(t *testing.T) {
	for name, entry := range map[string]spool.Entry{
		"verified":          verifiedEntry(),
		"unverified":        unverifiedEntry(),
		"no branch":         withBranch(verifiedEntry(), ""),
		"detached branch":   withBranch(unverifiedEntry(), "refs/pull/17/merge"),
		"uppercase-free ID": verifiedEntry(),
	} {
		if err := checkUploadedSourceIdentityIsStated(entry); err != nil {
			t.Fatalf("%s: refused a stated identity: %v", name, err)
		}
	}
}

func withBranch(entry spool.Entry, branch string) spool.Entry {
	entry.Source.Branch = branch
	return entry
}

func TestAnUnstatedIdentityIsRefusedBeforeItIsSent(t *testing.T) {
	cases := map[string]func(spool.Entry) spool.Entry{
		"no repository": func(e spool.Entry) spool.Entry {
			e.Source.Repository = ""
			return e
		},
		"no commit": func(e spool.Entry) spool.Entry {
			e.Source.CommitSHA = ""
			return e
		},
		"short commit": func(e spool.Entry) spool.Entry {
			e.Source.CommitSHA = aCommit[:39]
			return e
		},
		"non-hex commit": func(e spool.Entry) spool.Entry {
			e.Source.CommitSHA = strings.Replace(aCommit, "0", "g", 1)
			return e
		},
		"uppercase commit": func(e spool.Entry) spool.Entry {
			e.Source.CommitSHA = strings.ToUpper(aCommit)
			return e
		},
		"no verification word": func(e spool.Entry) spool.Entry {
			e.Source.Verification = ""
			return e
		},
		"invented verification word": func(e spool.Entry) spool.Entry {
			e.Source.Verification = "trusted"
			return e
		},
		"verified with no snapshot": func(e spool.Entry) spool.Entry {
			e.Source.SnapshotSHA256 = ""
			return e
		},
		"verified with a short snapshot": func(e spool.Entry) spool.Entry {
			e.Source.SnapshotSHA256 = aSnapshot[:63]
			return e
		},
	}
	for name, spoil := range cases {
		err := checkUploadedSourceIdentityIsStated(spoil(verifiedEntry()))
		if !errors.Is(err, ErrUnstatedSourceIdentity) {
			t.Fatalf("%s: accepted an unstated identity (err %v)", name, err)
		}
	}
}

// The rule the spool package comment states from the other end: unverified
// evidence "is never upgraded to trusted CI evidence during upload". A
// snapshot digest is the thing a verified record carries, so an unverified
// record carrying one is that upgrade written down.
func TestAnUnverifiedRecordMayNotCarryASnapshotDigest(t *testing.T) {
	entry := unverifiedEntry()
	entry.Source.SnapshotSHA256 = aSnapshot
	err := checkUploadedSourceIdentityIsStated(entry)
	if !errors.Is(err, ErrUnstatedSourceIdentity) {
		t.Fatalf("an unverified record was allowed to carry a snapshot digest: %v", err)
	}
}

// The refusal has to name the record's fault, because naming it is the whole
// reason this is refused here rather than by an HTTP status.
func TestTheRefusalNamesTheFieldAtFault(t *testing.T) {
	for word, want := range map[string]string{
		"":        "verification",
		"trusted": "trusted",
	} {
		entry := verifiedEntry()
		entry.Source.Verification = word
		err := checkUploadedSourceIdentityIsStated(entry)
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Fatalf("verification %q: refusal did not name the fault: %v", word, err)
		}
	}
	entry := verifiedEntry()
	entry.Source.Repository = ""
	if err := checkUploadedSourceIdentityIsStated(entry); err == nil ||
		!strings.Contains(err.Error(), "repository") {
		t.Fatalf("missing repository: refusal did not name the fault: %v", err)
	}
}

// This states the identity, and nothing else about the record. Whether the
// record is terminal, which schema version it claims, and whether its result
// is consistent are other doors' questions, and answering them here would put
// two readers on one rule.
func TestNothingButTheIdentityIsJudged(t *testing.T) {
	entry := verifiedEntry()
	entry.SchemaVersion = 0
	entry.ID = ""
	entry.Task = ""
	entry.SyncState = "running"
	entry.FinishedAt = nil
	entry.Result = nil
	if err := checkUploadedSourceIdentityIsStated(entry); err != nil {
		t.Fatalf("judged something other than the source identity: %v", err)
	}
}

// The same pair of rules the server applies to the verification word
// (server.offlineInput) and the store applies again before the insert
// (store.validateLocalRun), transcribed here and crossed over. If either end
// changes its mind about what a source identity is, this fails.
func TestThisDoorAppliesTheRuleTheIngestEndApplies(t *testing.T) {
	serverWouldAccept := func(source spool.SourceIdentity) bool {
		if source.Repository == "" || source.CommitSHA == "" {
			return false
		}
		if source.Verification != "verified" && source.Verification != "unverified" {
			return false
		}
		if source.Verification == "verified" && source.SnapshotSHA256 == "" {
			return false
		}
		return !(source.Verification == "unverified" && source.SnapshotSHA256 != "")
	}
	sources := []spool.SourceIdentity{
		{Repository: "r", CommitSHA: aCommit, Verification: "verified", SnapshotSHA256: aSnapshot},
		{Repository: "r", CommitSHA: aCommit, Verification: "unverified"},
		{Repository: "r", CommitSHA: aCommit, Verification: "unverified", SnapshotSHA256: aSnapshot},
		{Repository: "r", CommitSHA: aCommit, Verification: "verified"},
		{Repository: "r", CommitSHA: aCommit, Verification: "trusted", SnapshotSHA256: aSnapshot},
		{Repository: "", CommitSHA: aCommit, Verification: "verified", SnapshotSHA256: aSnapshot},
		{Repository: "r", CommitSHA: "", Verification: "verified", SnapshotSHA256: aSnapshot},
	}
	for i, source := range sources {
		entry := verifiedEntry()
		entry.Source = source
		accepted := checkUploadedSourceIdentityIsStated(entry) == nil
		if accepted && !serverWouldAccept(source) {
			t.Fatalf("source %d: this door accepted what the ingest end refuses", i)
		}
	}
}

func TestHexOfLengthHoldsItsBoundaries(t *testing.T) {
	for value, want := range map[string]bool{
		"":                                 false,
		"0":                                false,
		"00":                               true,
		"0g":                               false,
		"0A":                               false,
		"ff":                               true,
		"0123456789abcdef0123456789abcdef": false,
	} {
		if got := hexOfLength(value, 2); got != want {
			t.Fatalf("hexOfLength(%q, 2) = %v, want %v", value, got, want)
		}
	}
	if !hexOfLength(aCommit, 40) || !hexOfLength(aSnapshot, 64) {
		t.Fatal("hexOfLength refused a well-formed digest")
	}
	if hexOfLength(aCommit, 64) || hexOfLength(aSnapshot, 40) {
		t.Fatal("hexOfLength ignored the length it was given")
	}
}
