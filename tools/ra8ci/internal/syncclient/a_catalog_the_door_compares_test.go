package syncclient

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// frozenAgainst builds a terminal record naming digest as the catalog it was
// begun under.
func frozenAgainst(digest string) spool.Entry {
	started := time.Date(2026, 9, 27, 14, 40, 0, 0, time.UTC)
	finished := started.Add(30 * time.Second)
	return spool.Entry{
		SchemaVersion: 2,
		ID:            strings.Repeat("e", 32),
		Task:          "unit-tests",
		CatalogDigest: digest,
		StartedAt:     started,
		FinishedAt:    &finished,
		SyncState:     "unsynced",
		Result: &executor.Result{
			TaskName:  "unit-tests",
			StartedAt: started,
			EndedAt:   finished,
		},
	}
}

func TestADigestTheStoreWouldFileIsUploaded(t *testing.T) {
	for _, digest := range []string{
		strings.Repeat("0", 64),
		strings.Repeat("f", 64),
		strings.Repeat("ab12", 16),
	} {
		if err := checkUploadedCatalogDigestIsStated(frozenAgainst(digest)); err != nil {
			t.Fatalf("digest %q refused: %v", digest, err)
		}
	}
}

func TestARecordNamingNoCatalogIsRefused(t *testing.T) {
	err := checkUploadedCatalogDigestIsStated(frozenAgainst(""))
	if !errors.Is(err, ErrUnstatedCatalogDigest) {
		t.Fatalf("empty digest accepted: %v", err)
	}
}

func TestADigestOfTheWrongLengthIsRefused(t *testing.T) {
	for _, digest := range []string{
		strings.Repeat("a", 63),
		strings.Repeat("a", 65),
		strings.Repeat("a", 40),
	} {
		if err := checkUploadedCatalogDigestIsStated(frozenAgainst(digest)); !errors.Is(err, ErrUnstatedCatalogDigest) {
			t.Fatalf("digest of %d chars accepted: %v", len(digest), err)
		}
	}
}

// The store's rule is ^[0-9a-f]{64}$, so an upper-case digest is refused there
// however well formed it looks here.
func TestAnUppercaseDigestIsRefused(t *testing.T) {
	err := checkUploadedCatalogDigestIsStated(frozenAgainst(strings.Repeat("AB12", 16)))
	if !errors.Is(err, ErrUnstatedCatalogDigest) {
		t.Fatalf("upper-case digest accepted: %v", err)
	}
}

func TestADigestThatIsNotHexIsRefused(t *testing.T) {
	for _, digest := range []string{
		strings.Repeat("g", 64),
		strings.Repeat("a", 63) + " ",
		strings.Repeat("a", 63) + "\x00",
	} {
		if err := checkUploadedCatalogDigestIsStated(frozenAgainst(digest)); !errors.Is(err, ErrUnstatedCatalogDigest) {
			t.Fatalf("non-hex digest %q accepted: %v", digest, err)
		}
	}
}

// The door judges the shape of the digest, never which catalog it names: two
// well-formed digests that cannot both be this server's are both uploaded, and
// the disagreement is left to the only party holding both catalogs.
func TestTheDoorDoesNotChooseACatalog(t *testing.T) {
	first := strings.Repeat("1", 64)
	second := strings.Repeat("2", 64)
	if err := checkUploadedCatalogDigestIsStated(frozenAgainst(first)); err != nil {
		t.Fatalf("first digest refused: %v", err)
	}
	if err := checkUploadedCatalogDigestIsStated(frozenAgainst(second)); err != nil {
		t.Fatalf("second digest refused: %v", err)
	}
}

func TestTheRefusalQuotesTheDigest(t *testing.T) {
	err := checkUploadedCatalogDigestIsStated(frozenAgainst("nope"))
	if err == nil || !strings.Contains(err.Error(), `"nope"`) {
		t.Fatalf("refusal did not quote the digest: %v", err)
	}
}
