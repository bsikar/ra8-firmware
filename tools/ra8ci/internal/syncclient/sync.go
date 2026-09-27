// Package syncclient uploads already-finished local task evidence to the
// control plane. It never asks the server to execute a task.
package syncclient

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type Report struct {
	Synced      int
	Quarantined int // schema-v1 records remain unsynced, never reclassified
}

// SyncPending sends schema-v2 terminal records over HTTPS. It writes a local
// synced marker only after the server returns a matching durable receipt.
// Redirects are forbidden so the outbox cannot be sent to another origin.
func SyncPending(ctx context.Context, outbox *spool.Spool, baseURL string, client *http.Client) (Report, error) {
	if outbox == nil || client == nil {
		return Report{}, errors.New("offline sync requires outbox and HTTP client")
	}
	endpoint, err := syncEndpoint(baseURL)
	if err != nil {
		return Report{}, err
	}
	entries, err := outbox.Pending()
	if err != nil {
		return Report{}, err
	}
	safeClient := *client
	safeClient.CheckRedirect = func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}
	var report Report
	claimed := make(map[string]string, len(entries))
	for _, entry := range entries {
		if err := checkSchemaVersionIsKnown(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		if entry.SchemaVersion != uploadableSchemaVersion {
			report.Quarantined++
			continue
		}
		if err := checkUploadedSourceIdentityIsStated(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Asked beside the identity door and before the record is marshalled:
		// both judge what the record says about itself, and neither needs the
		// bytes to answer.
		if err := checkUploadedEnvelopeIsReadable(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Last of the three record-only doors, for the same reason as the two
		// above: it judges what the record says about itself, needs neither the
		// bytes nor a catalog to answer, and the server refuses the pair with
		// the same opaque 400 that stops the sweep.
		if err := checkUploadedResultNamesItsTask(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Beside the three doors above rather than after the marshal: the
		// codes are stated by the record, need neither the bytes nor a
		// catalog to judge, and the server refuses them with the same opaque
		// 400 that ends the sweep.
		if err := checkUploadedExitsAreOnesARunnerCouldReport(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Last of the record-only doors, beside the exits for the same
		// reason: a step's log evidence is stated by the record, needs
		// neither the bytes nor a catalog to judge, and the server refuses it
		// with the same opaque 400 that ends the sweep.
		if err := checkUploadedLogEvidenceWasMeasured(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Beside the record-only doors above, for the same reason: the
		// digest is stated by the record, its SHAPE needs neither the bytes
		// nor a catalog to judge, and the store refuses a shapeless one with
		// the same opaque 400 that ends the sweep.
		if err := checkUploadedCatalogDigestIsStated(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Beside the digest door for the same reason: the tier, the scope
		// and the deadline are stated by the record, the columns that hold
		// them carry CHECK constraints needing no catalog to read, and an
		// unfilable one is the same opaque 400 that ends the sweep.
		if err := checkUploadedClassificationIsOneThePlaneFiles(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Beside the classification door for the same reason: the task
		// name is stated by the record, the column that holds it bounds
		// its length and its text with no catalog to read, and a name the
		// plane will not file is the same opaque 400 that ends the sweep.
		if err := checkUploadedTaskNameIsOneThePlaneFiles(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Last of the record-only doors, beside the task name for the same
		// reason: the arguments are stated by the record, the jsonb column
		// that holds them bounds their count and their text with no catalog
		// to read, and one the plane will not file is the same opaque 400
		// that ends the sweep.
		if err := checkUploadedArgumentsAreOnesThePlaneWillFile(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Last of the record-only doors, beside the arguments for the same
		// reason: the steps are stated by the record, the rows that hold
		// them bound their count and their key text with no catalog to
		// read, and steps the plane will not file are the same opaque 400
		// that ends the sweep.
		if err := checkUploadedStepsAreOnesThePlaneWillFile(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Beside the step door for the same reason: how a step stopped is
		// stated by the record, the pair needs no catalog to judge, and a
		// step the plane cannot file is the same opaque 400 that ends the
		// sweep.
		if err := checkUploadedStepEndingsAreOnesThePlaneFiles(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Last of the step doors: when each step ran is stated by the
		// record, the rows that hold a step bound its stamps and its
		// duration with no catalog to read, and a window the plane will
		// not file is the same opaque 400 that ends the sweep.
		if err := checkUploadedStepWindowsWereMeasured(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Beside the step windows, on the pair the executor wrote rather
		// than the pair the spool did: an execution claiming to have run
		// outside the window this host observed needs no catalog to
		// refuse, and is the same opaque 400 that ends the sweep.
		if err := checkUploadedAttemptWindowFitsTheRecord(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Beside the identity door rather than at it: that door asks whether
		// the record claims a source at all, this one asks whether the two
		// free-text halves of the claim are text the plane has a column for,
		// and neither needs the bytes nor a catalog to answer.
		if err := checkUploadedSourceNamesOneThePlaneFiles(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		body, err := json.Marshal(entry)
		if err != nil {
			return report, err
		}
		// Asked once the bytes exist and before any of them are sent: this is
		// the first point the record's real size is known, and the last point
		// before the sweep's outcome is decided by a door it cannot see.
		if err := checkUploadedRecordFitsTheOfflineDoor(body); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		// Asked AFTER the size door rather than beside the record-only
		// doors above, and deliberately: a failure message is the one
		// field wide enough to carry a record over the offline door by
		// itself, and a record oversized on the whole should be reported
		// as that rather than under one of its fields. Everything this
		// door refuses is therefore a record the plane would otherwise
		// have taken the bytes of and then refused at the column.
		if err := checkUploadedExecutorErrorIsOneThePlaneWillFile(entry); err != nil {
			return report, fmt.Errorf("local %s: %w", entry.ID, err)
		}
		canonical, err := catalog.CanonicalJSON(body)
		if err != nil {
			return report, err
		}
		sum := sha256.Sum256(canonical)
		digest := hex.EncodeToString(sum[:])
		request, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, bytes.NewReader(body))
		if err != nil {
			return report, err
		}
		request.Header.Set("Content-Type", "application/json")
		response, err := safeClient.Do(request)
		if err != nil {
			return report, fmt.Errorf("upload local %s: %w", entry.ID, err)
		}
		limited := io.LimitReader(response.Body, 4097)
		responseBody, readErr := io.ReadAll(limited)
		closeErr := response.Body.Close()
		if readErr != nil || closeErr != nil || len(responseBody) > 4096 {
			return report, fmt.Errorf("read local %s receipt: %v / %v", entry.ID, readErr, closeErr)
		}
		if response.StatusCode != http.StatusOK {
			return report, fmt.Errorf("upload local %s returned HTTP %d", entry.ID, response.StatusCode)
		}
		var receipt store.LocalRunReceipt
		decoder := json.NewDecoder(bytes.NewReader(responseBody))
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&receipt); err != nil {
			return report, fmt.Errorf("decode local %s receipt: %w", entry.ID, err)
		}
		var extra any
		if err := decoder.Decode(&extra); !errors.Is(err, io.EOF) ||
			receipt.LocalID != entry.ID || receipt.PayloadSHA256 != digest || !store.ValidID(receipt.LocalRunID) {
			return report, fmt.Errorf("local %s receipt did not match durable request", entry.ID)
		}
		if err := checkDurableRunIsUnclaimed(claimed, entry.ID, receipt); err != nil {
			return report, fmt.Errorf("local %s receipt: %w", entry.ID, err)
		}
		if err := outbox.MarkSynced(entry.ID, receipt.LocalRunID); err != nil {
			return report, fmt.Errorf("persist local %s receipt: %w", entry.ID, err)
		}
		report.Synced++
	}
	return report, nil
}
