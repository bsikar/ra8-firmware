// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"strings"
	"testing"
)

func grantNaming(name string) Assignment {
	grant := sampleAssignment()
	grant.Task.Name = name
	return grant
}

func TestEveryNameTheCatalogDeclaresIsAccepted(t *testing.T) {
	// Real declared names, including the two longest and one that is a single
	// character, so the rule is measured against the catalog rather than
	// against the shape it was written from.
	for _, name := range []string{"selftest", "format-check", "rewrite-path", "legacy-make",
		"wave-references", "tz-boundary", "nsc-veneer-defs", "stub-crypto-guard", "x", "9"} {
		if !taskNameNamesAReviewedTask(name) {
			t.Fatalf("refused a name a reviewed task could carry: %q", name)
		}
	}
}

func TestAnEmptyTaskNameIsRefused(t *testing.T) {
	if taskNameNamesAReviewedTask("") {
		t.Fatal("accepted a grant naming nothing")
	}
}

func TestATaskNameCarryingAControlCharacterIsRefused(t *testing.T) {
	for _, name := range []string{"selftest\n", "self\ntest", "selftest\x00", "selftest\t",
		"\x1b[31mselftest"} {
		if taskNameNamesAReviewedTask(name) {
			t.Fatalf("accepted a control character in %q", name)
		}
	}
}

func TestATaskNameThatIsNotValidUTF8IsRefused(t *testing.T) {
	if taskNameNamesAReviewedTask("self\xfftest") {
		t.Fatal("accepted bytes that are not valid UTF-8")
	}
}

func TestSurroundingWhitespaceInATaskNameIsRefused(t *testing.T) {
	for _, name := range []string{" selftest", "selftest ", "self test", "\u3000selftest"} {
		if taskNameNamesAReviewedTask(name) {
			t.Fatalf("accepted whitespace in %q", name)
		}
	}
}

func TestATaskNameOutsideTheCatalogAlphabetIsRefused(t *testing.T) {
	for _, name := range []string{"Selftest", "SELFTEST", "self_test", "self.test",
		"self/test", "../selftest", "self:test", "self;rm -rf /"} {
		if taskNameNamesAReviewedTask(name) {
			t.Fatalf("accepted a name outside the catalog alphabet: %q", name)
		}
	}
}

func TestATaskNameAtTheBoundIsAcceptedAndPastItRefused(t *testing.T) {
	if !taskNameNamesAReviewedTask(strings.Repeat("a", maxTaskNameBytes)) {
		t.Fatal("refused a name exactly at the bound")
	}
	if taskNameNamesAReviewedTask(strings.Repeat("a", maxTaskNameBytes+1)) {
		t.Fatal("accepted a name past the bound")
	}
}

func TestAnUnboundedTaskNameIsRefused(t *testing.T) {
	if taskNameNamesAReviewedTask(strings.Repeat("selftest", 4096)) {
		t.Fatal("accepted a name no reviewed task could carry")
	}
}

func TestAGrantNamingARealTaskIsStillAccepted(t *testing.T) {
	if err := grantNaming("format-check").Validate(); err != nil {
		t.Fatalf("refused an honest grant: %v", err)
	}
}

func TestAGrantIsRefusedForAnUnnameableTask(t *testing.T) {
	for _, name := range []string{"", "format check", "Format-Check", "format-check\n",
		strings.Repeat("a", maxTaskNameBytes+1)} {
		if err := grantNaming(name).Validate(); err == nil {
			t.Fatalf("a grant carried the task name %q", name)
		}
	}
}
