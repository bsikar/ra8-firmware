package store

import (
	"context"
	"fmt"
	"time"
)

// SlowTask summarizes completed, evidence-backed attempts. Resource fields
// describe host-wide context and are not task-attributed CPU measurements.
type SlowTask struct {
	Name             string   `json:"name"`
	HostClass        string   `json:"host_class"`
	Samples          int64    `json:"samples"`
	MedianSeconds    float64  `json:"median_seconds"`
	P95Seconds       float64  `json:"p95_seconds"`
	MaximumSeconds   float64  `json:"maximum_seconds"`
	MeanStartLoad    float64  `json:"mean_start_load"`
	HostOS           string   `json:"host_os"`
	HostLoadKind     string   `json:"host_load_kind"`
	ResourceSamples  int64    `json:"resource_samples"`
	MeanHostLoad     float64  `json:"mean_host_load"`
	PeakHostLoad     float64  `json:"peak_host_load"`
	MeanRAMFreeBytes float64  `json:"mean_ram_free_bytes"`
	MeanHostCores    float64  `json:"mean_host_cores"`
	MeanCPUBusyPct   *float64 `json:"mean_cpu_busy_percent,omitempty"`
	MeanLoadPerCore  *float64 `json:"mean_load_per_core,omitempty"`
	MeanRAMUsedPct   *float64 `json:"mean_ram_used_percent,omitempty"`
}

// SlowTasks ranks task names by median wall time over a bounded UTC window.
// The repository filter is mandatory to avoid cross-repository disclosure.
func (s *Store) SlowTasks(ctx context.Context, repository string, since time.Time, limit int) ([]SlowTask, error) {
	if s == nil || s.pool == nil {
		return nil, ErrUnavailable
	}
	if repository == "" || len(repository) > 512 || since.IsZero() || limit < 1 || limit > 500 {
		return nil, fmt.Errorf("%w: slow report parameters", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `WITH per_attempt_resources AS (
		SELECT attempt_id, host_os, host_load_kind, COUNT(*)::bigint AS sample_count,
			AVG(host_load)::double precision AS mean_host_load,
			MAX(host_load)::double precision AS peak_host_load,
			AVG(host_ram_available_bytes)::double precision AS mean_ram_free
		FROM resource_samples WHERE host_os IS NOT NULL AND host_load_kind IS NOT NULL
		GROUP BY attempt_id, host_os, host_load_kind
	)
	SELECT t.name, t.host_class, COUNT(DISTINCT a.id)::bigint,
		percentile_cont(0.5) WITHIN GROUP (ORDER BY EXTRACT(EPOCH FROM (a.ended_at-a.started_at))::double precision),
		percentile_cont(0.95) WITHIN GROUP (ORDER BY EXTRACT(EPOCH FROM (a.ended_at-a.started_at))::double precision),
		MAX(EXTRACT(EPOCH FROM (a.ended_at-a.started_at)))::double precision,
		COALESCE(AVG(a.host_load), 0)::double precision,
		COALESCE(pr.host_os, 'unknown'), COALESCE(pr.host_load_kind, 'unavailable'),
		COALESCE(SUM(pr.sample_count), 0)::bigint,
		COALESCE(AVG(pr.mean_host_load), 0)::double precision,
		COALESCE(MAX(pr.peak_host_load), 0)::double precision,
		COALESCE(AVG(pr.mean_ram_free), 0)::double precision,
		COALESCE(AVG(a.host_cores), 0)::double precision,
		CASE WHEN pr.host_load_kind='cpu_busy_equivalent' THEN AVG(pr.mean_host_load)::double precision END,
		CASE WHEN pr.host_load_kind='linux_load1' THEN AVG(pr.mean_host_load/NULLIF(a.host_cores,0))::double precision END,
		CASE WHEN AVG(pr.mean_ram_free) IS NOT NULL AND AVG(a.host_ram_bytes) > 0
			THEN (100*(1-AVG(pr.mean_ram_free/a.host_ram_bytes)))::double precision END
		FROM runs r
		JOIN tasks t ON t.run_id=r.id
		JOIN task_attempts a ON a.task_id=t.id
		LEFT JOIN per_attempt_resources pr ON pr.attempt_id=a.id
		WHERE r.repository=$1 AND r.created_at >= $2 AND a.ended_at IS NOT NULL
		AND a.started_at IS NOT NULL AND a.evidence_complete=true
		GROUP BY t.name, t.host_class, pr.host_os, pr.host_load_kind
		ORDER BY 4 DESC, t.name ASC, t.host_class ASC, pr.host_os ASC LIMIT $3`, repository, since.UTC(), limit)
	if err != nil {
		return nil, fmt.Errorf("%w: query slow tasks: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	result := make([]SlowTask, 0)
	for rows.Next() {
		var item SlowTask
		if err := rows.Scan(&item.Name, &item.HostClass, &item.Samples, &item.MedianSeconds, &item.P95Seconds, &item.MaximumSeconds, &item.MeanStartLoad,
			&item.HostOS, &item.HostLoadKind, &item.ResourceSamples, &item.MeanHostLoad, &item.PeakHostLoad, &item.MeanRAMFreeBytes,
			&item.MeanHostCores, &item.MeanCPUBusyPct, &item.MeanLoadPerCore, &item.MeanRAMUsedPct); err != nil {
			return nil, fmt.Errorf("%w: scan slow task: %v", ErrUnavailable, err)
		}
		result = append(result, item)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: slow task rows: %v", ErrUnavailable, err)
	}
	return result, nil
}
