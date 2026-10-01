// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package migrations

import (
	"context"
	"io/fs"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// Apply judges the embedded migration set against a live database: a gap, a
// misnamed file, or a set that stops short of the binary's schema version is
// only discovered when a deployment already has a transaction open. None of
// that needs Postgres to be true, so it is checked here instead, where a
// broken set fails the build.

// migrationName is the shape Apply's own SplitN plus Atoi will accept.
var migrationName = regexp.MustCompile(`^[0-9]{4}_[a-z0-9_]+\.sql$`)

func embeddedMigrations(t *testing.T) []string {
	t.Helper()
	entries, err := fs.Glob(sqlFiles, "*.sql")
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) == 0 {
		t.Fatal("no migrations are embedded")
	}
	sort.Strings(entries)
	return entries
}

// Every embedded file has to split the way Apply splits it, or Apply refuses
// the whole set with "invalid migration name".
func TestEveryEmbeddedMigrationIsNamedTheWayApplyReadsIt(t *testing.T) {
	for _, name := range embeddedMigrations(t) {
		if !migrationName.MatchString(name) {
			t.Errorf("%q is not a four-digit version, an underscore, and a lowercase name", name)
			continue
		}
		parts := strings.SplitN(name, "_", 2)
		if len(parts) != 2 {
			t.Errorf("%q does not split into a version and a name", name)
			continue
		}
		version, err := strconv.Atoi(parts[0])
		if err != nil || version < 1 || version > currentVersion {
			t.Errorf("%q carries no version Apply would accept", name)
		}
	}
}

// Apply walks the sorted set expecting each version to be exactly one past the
// last, so a gap or a repeat stops a deployment. Sorting by name has to agree
// with sorting by version for that walk to hold, which is what the zero
// padding buys.
func TestTheEmbeddedMigrationsRunFromOneWithNoGapOrRepeat(t *testing.T) {
	entries := embeddedMigrations(t)
	for i, name := range entries {
		version, err := strconv.Atoi(strings.SplitN(name, "_", 2)[0])
		if err != nil {
			t.Fatalf("%q carries no version", name)
		}
		if want := i + 1; version != want {
			t.Fatalf("sorted position %d is version %d, so the set has a gap or a repeat at %q", want, version, name)
		}
	}
}

// Apply refuses to commit unless the set reaches the binary's own schema
// version, so the constant and the files have to be raised together.
func TestTheEmbeddedSetReachesTheVersionTheBinaryExpects(t *testing.T) {
	entries := embeddedMigrations(t)
	highest, err := strconv.Atoi(strings.SplitN(entries[len(entries)-1], "_", 2)[0])
	if err != nil {
		t.Fatal(err)
	}
	if highest != currentVersion {
		t.Fatalf("the highest embedded migration is %d but the binary expects %d", highest, currentVersion)
	}
	if CurrentVersion() != currentVersion {
		t.Fatalf("CurrentVersion reported %d, want %d", CurrentVersion(), currentVersion)
	}
}

// An empty migration would be recorded as applied while changing nothing,
// which is worse than a missing file: the ledger would claim a schema the
// database does not have.
func TestNoEmbeddedMigrationIsEmpty(t *testing.T) {
	for _, name := range embeddedMigrations(t) {
		body, err := sqlFiles.ReadFile(name)
		if err != nil {
			t.Errorf("read %q: %v", name, err)
			continue
		}
		if len(strings.TrimSpace(string(body))) == 0 {
			t.Errorf("%q is empty, so applying it would record a schema change that did not happen", name)
		}
	}
}

// The one refusal Apply can answer without a database, pinned so a caller
// that forgot its pool gets a named error rather than a nil dereference.
func TestApplyRefusesAMissingPool(t *testing.T) {
	err := Apply(context.Background(), nil)
	if err == nil {
		t.Fatal("Apply accepted a nil pool")
	}
	if !strings.Contains(err.Error(), "migration pool is nil") {
		t.Fatalf("Apply refused a nil pool without saying so: %v", err)
	}
}
