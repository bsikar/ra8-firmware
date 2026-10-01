package store

import (
	"context"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/jackc/pgx/v5"
)

// recordResourceSample appends server-timestamped host context for one fenced
// attempt. Load values retain their OS-specific meaning and are not task CPU
// consumption measurements.
func recordResourceSample(ctx context.Context, tx pgx.Tx, attemptID string, facts protocol.HostFacts) error {
	if tx == nil || !ValidID(attemptID) || facts.Validate() != nil {
		return ErrInvalid
	}
	_, err := tx.Exec(ctx, `WITH sample_clock AS (
		SELECT a.started_at, clock_timestamp() AS sampled_at,
			COALESCE(MAX(s.sample_no)+1,0) AS sample_no,
			COALESCE(MAX(s.monotonic_offset_ns),0) AS previous_offset_ns
		FROM task_attempts a LEFT JOIN resource_samples s ON s.attempt_id=a.id
		WHERE a.id=$1 GROUP BY a.started_at
	)
	INSERT INTO resource_samples
		(attempt_id,sample_no,monotonic_offset_ns,interval_ns,host_load,
		host_ram_available_bytes,host_os,host_load_kind)
	SELECT $1,sample_no,
		GREATEST(previous_offset_ns+1,
			GREATEST(0,(EXTRACT(EPOCH FROM (sampled_at-started_at))*1000000000)::bigint)),
		GREATEST(1,GREATEST(previous_offset_ns+1,
			GREATEST(0,(EXTRACT(EPOCH FROM (sampled_at-started_at))*1000000000)::bigint))-previous_offset_ns),
		$2,$3,$4,$5 FROM sample_clock`, attemptID, facts.Load1,
		facts.RAMFreeBytes, facts.OS, facts.LoadKind)
	if err != nil {
		return fmt.Errorf("%w: insert resource sample: %v", ErrUnavailable, err)
	}
	return nil
}
