package server

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// ingestOffline accepts historical local evidence only. It never creates a
// scheduled task or runs code on the server. The mTLS peer, not the JSON,
// supplies the principal written to durable history.
func (s *Server) ingestOffline(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("Content-Type") != "application/json" {
		problem(w, http.StatusUnsupportedMediaType, "invalid_argument", "content type must be application/json", false)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 256<<10)
	defer r.Body.Close()
	raw, err := io.ReadAll(r.Body)
	if err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "offline record exceeds limit or is unreadable", false)
		return
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	var entry spool.Entry
	if err := decoder.Decode(&entry); err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid offline record", false)
		return
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		problem(w, http.StatusBadRequest, "invalid_argument", "trailing JSON data", false)
		return
	}
	principal, err := s.auth.Authorize(r, entry.Source.Repository, "submit")
	if err != nil {
		s.deny(w, r, "local_run.ingest", entry.Source.Repository, err)
		return
	}
	canonical, err := catalog.CanonicalJSON(raw)
	if err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "offline JSON cannot be canonicalized", false)
		return
	}
	sum := sha256.Sum256(canonical)
	digest := hex.EncodeToString(sum[:])
	previous, err := s.store.LookupLocalRunReceipt(r.Context(), principal, entry.ID, digest)
	if err == nil {
		writeJSON(w, http.StatusOK, previous)
		return
	}
	if !errors.Is(err, store.ErrNotFound) {
		writeStoreError(w, err)
		return
	}
	in, err := offlineInput(entry, s.catalog)
	if err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "offline record lacks reviewed metadata or result evidence", false)
		return
	}
	in.PrincipalID = principal
	in.PayloadSHA256 = digest
	receipt, err := s.store.IngestLocalRun(r.Context(), in)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, receipt)
}

func offlineInput(entry spool.Entry, cat *catalog.Catalog) (store.LocalRunInput, error) {
	if cat == nil || entry.SchemaVersion != 2 || entry.SyncState != "unsynced" ||
		entry.FinishedAt == nil || entry.Result == nil || entry.CatalogDigest != cat.Digest() ||
		entry.Source.Repository == "" || entry.Source.CommitSHA == "" {
		return store.LocalRunInput{}, store.ErrInvalid
	}
	if err := checkLocalEnvelopeIsStated(entry); err != nil {
		return store.LocalRunInput{}, err
	}
	definition, found := cat.Task(entry.Task)
	if !found || !definition.IsSafeLocal() || definition.Tier != entry.Tier ||
		definition.Scope != entry.Scope || definition.DeadlineSeconds != entry.DeadlineSeconds ||
		definition.ValidateArguments(entry.Args) != nil ||
		entry.Result.TaskName != "" && entry.Result.TaskName != entry.Task {
		return store.LocalRunInput{}, store.ErrInvalid
	}
	if entry.Source.Verification != "verified" && entry.Source.Verification != "unverified" ||
		entry.Source.Verification == "verified" && entry.Source.SnapshotSHA256 == "" ||
		entry.Source.Verification == "unverified" && entry.Source.SnapshotSHA256 != "" {
		return store.LocalRunInput{}, store.ErrInvalid
	}
	in := store.LocalRunInput{
		LocalID: entry.ID, SourceVerification: entry.Source.Verification,
		Repository: entry.Source.Repository, Branch: entry.Source.Branch,
		CommitSHA: entry.Source.CommitSHA, SnapshotSHA256: entry.Source.SnapshotSHA256,
		CatalogSHA256: entry.CatalogDigest, TaskName: entry.Task, Tier: entry.Tier,
		Scope: entry.Scope, DeadlineSeconds: entry.DeadlineSeconds,
		Arguments: append([]string(nil), entry.Args...), StartedAt: entry.StartedAt,
		FinishedAt: *entry.FinishedAt, DurationNS: entry.FinishedAt.Sub(entry.StartedAt).Nanoseconds(),
		ChildExitCode: entry.Result.ExitCode, ExecutorError: entry.Error,
	}
	switch {
	case entry.Error != "" || entry.Result.StartedAt.IsZero() || entry.Result.EndedAt.IsZero():
		in.Result = "incomplete_evidence"
	case entry.Result.TimedOut:
		in.Result = "timed_out"
	case entry.Result.Cancelled:
		in.Result = "cancelled"
	case entry.Result.ExitCode == 0:
		in.Result = "succeeded"
	default:
		in.Result = "failed"
	}
	if !entry.Result.StartedAt.IsZero() &&
		(entry.Result.StartedAt.Before(entry.StartedAt) || entry.Result.EndedAt.After(*entry.FinishedAt)) {
		return store.LocalRunInput{}, fmt.Errorf("%w: executor timestamps outside local envelope", store.ErrInvalid)
	}
	if len(entry.Result.Steps) > len(definition.Steps) {
		return store.LocalRunInput{}, store.ErrInvalid
	}
	for i, step := range entry.Result.Steps {
		if step.Name != definition.Steps[i].Name {
			return store.LocalRunInput{}, store.ErrInvalid
		}
		in.Steps = append(in.Steps, store.LocalStepInput{
			Key: step.Name, Ordinal: i, StartedAt: step.StartedAt,
			EndedAt: step.EndedAt, DurationNS: int64(step.Duration), ExitCode: step.ExitCode,
			TimedOut: step.TimedOut, Cancelled: step.Cancelled,
			StdoutSHA256: step.StdoutSHA256, StderrSHA256: step.StderrSHA256,
			StdoutBytes: step.StdoutBytes, StderrBytes: step.StderrBytes,
		})
		if in.Result == "succeeded" && (step.ExitCode != 0 || step.TimedOut || step.Cancelled) {
			in.Result = "incomplete_evidence"
		}
	}
	if in.Result == "succeeded" && len(in.Steps) != len(definition.Steps) {
		in.Result = "incomplete_evidence"
	}
	if entry.StartedAt.After(*entry.FinishedAt) || entry.FinishedAt.After(time.Now().Add(5*time.Minute)) {
		return store.LocalRunInput{}, store.ErrInvalid
	}
	return in, nil
}

// maxLocalEnvelope caps the wall-clock span a spooled record may state about
// itself. A reviewed task's deadline is at most 86400 seconds (the ceiling
// catalog.ValidateTask holds every task to), so no honest attempt at one can
// span longer than that; the extra hour is slack for the spool write and for
// skew between the two stamps, which are taken by the local host and not by
// the server that reads them.
const maxLocalEnvelope = 25 * time.Hour

// checkLocalEnvelopeIsStated holds a spooled record's own envelope to two
// stamps that can be subtracted.
//
// Neither stamp is validated anywhere else. A record arrives as client JSON, so
// an absent started_at is the zero time rather than a missing field, and the
// only stamp rule below is that the start is not AFTER the finish, which the
// zero time passes trivially. What that buys the record is not a merely odd
// duration: time.Time.Sub saturates, so FinishedAt.Sub(zero) is exactly
// math.MaxInt64 nanoseconds, and that is what lands in the local run's
// duration_ns in durable history, for a run the record itself never claimed to
// have started. A start far enough in the past saturates the same way with
// both stamps set, so the span is bounded here too rather than only checked
// for a zero.
//
// The existing "start is not after finish" refusal at the end of offlineInput
// is left where it is: it states a different thing (the stamps are in order),
// and one rule per refusal is how that function already reads.
func checkLocalEnvelopeIsStated(entry spool.Entry) error {
	if entry.FinishedAt == nil || entry.FinishedAt.IsZero() {
		return fmt.Errorf("%w: the spooled record states no finish", store.ErrInvalid)
	}
	if entry.StartedAt.IsZero() {
		return fmt.Errorf("%w: the spooled record states no start", store.ErrInvalid)
	}
	if span := entry.FinishedAt.Sub(entry.StartedAt); span > maxLocalEnvelope {
		return fmt.Errorf("%w: the spooled record's envelope spans %s, longer than any reviewed deadline",
			store.ErrInvalid, span)
	}
	return nil
}
