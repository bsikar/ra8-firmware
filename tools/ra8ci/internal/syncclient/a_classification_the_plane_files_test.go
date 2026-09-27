package syncclient

import (
	"errors"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// classified builds a terminal record filed under tier, scope and deadline.
func classified(tier, scope string, deadline int) spool.Entry {
	started := time.Date(2026, 9, 27, 14, 45, 0, 0, time.UTC)
	finished := started.Add(time.Minute)
	return spool.Entry{
		SchemaVersion:   2,
		ID:              strings.Repeat("c", 32),
		Task:            "unit-tests",
		CatalogDigest:   strings.Repeat("a", 64),
		Tier:            tier,
		Scope:           scope,
		DeadlineSeconds: deadline,
		StartedAt:       started,
		FinishedAt:      &finished,
		SyncState:       "unsynced",
		Result:          &executor.Result{TaskName: "unit-tests", StartedAt: started, EndedAt: finished},
	}
}

func TestEveryClassificationTheColumnsHoldIsUploaded(t *testing.T) {
	for _, tier := range []string{"required", "optional", "nightly"} {
		for _, scope := range []string{"safe-local-read-only", "safe-local-write-working-tree"} {
			for _, deadline := range []int{1, 900, 86400} {
				entry := classified(tier, scope, deadline)
				if err := checkUploadedClassificationIsOneThePlaneFiles(entry); err != nil {
					t.Fatalf("%s/%s/%ds refused: %v", tier, scope, deadline, err)
				}
			}
		}
	}
}

func TestATierNoColumnHoldsIsRefused(t *testing.T) {
	for _, tier := range []string{"", "urgent", "Required", "required "} {
		err := checkUploadedClassificationIsOneThePlaneFiles(classified(tier, "safe-local-read-only", 60))
		if !errors.Is(err, ErrUnfilableClassification) {
			t.Fatalf("tier %q accepted: %v", tier, err)
		}
	}
}

func TestAScopeNoColumnHoldsIsRefused(t *testing.T) {
	for _, scope := range []string{"", "safe-local", "safe-local-write", "unsafe-remote"} {
		err := checkUploadedClassificationIsOneThePlaneFiles(classified("required", scope, 60))
		if !errors.Is(err, ErrUnfilableClassification) {
			t.Fatalf("scope %q accepted: %v", scope, err)
		}
	}
}

func TestADeadlineOutsideTheColumnIsRefused(t *testing.T) {
	for _, deadline := range []int{0, -1, 86401, math.MaxInt32} {
		err := checkUploadedClassificationIsOneThePlaneFiles(classified("required", "safe-local-read-only", deadline))
		if !errors.Is(err, ErrUnfilableClassification) {
			t.Fatalf("deadline %ds accepted: %v", deadline, err)
		}
	}
}

// A run that took longer than the deadline it was begun under is ordinary
// evidence: the column holds the deadline, not a measurement of the run.
func TestARunThatOverranItsDeadlineIsStillUploaded(t *testing.T) {
	entry := classified("required", "safe-local-read-only", 1)
	if err := checkUploadedClassificationIsOneThePlaneFiles(entry); err != nil {
		t.Fatalf("overrunning record refused: %v", err)
	}
}

func TestTheRefusalNamesTheClassification(t *testing.T) {
	err := checkUploadedClassificationIsOneThePlaneFiles(classified("urgent", "safe-local-read-only", 60))
	if err == nil || !strings.Contains(err.Error(), `"urgent"`) {
		t.Fatalf("refusal did not name the tier: %v", err)
	}
	err = checkUploadedClassificationIsOneThePlaneFiles(classified("required", "safe-local-read-only", 0))
	if err == nil || !strings.Contains(err.Error(), "86400") {
		t.Fatalf("refusal did not name the range: %v", err)
	}
}
