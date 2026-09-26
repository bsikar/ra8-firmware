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
		if entry.SchemaVersion != 2 {
			report.Quarantined++
			continue
		}
		body, err := json.Marshal(entry)
		if err != nil {
			return report, err
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
