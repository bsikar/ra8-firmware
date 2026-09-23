package store

import (
	"encoding/json"
	"errors"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

var (
	ErrInvalid     = errors.New("invalid argument")
	ErrConflict    = errors.New("conflict")
	ErrNotFound    = errors.New("not found")
	ErrDenied      = errors.New("denied")
	ErrUnavailable = errors.New("database unavailable")
)

// CreateRunInput is immutable admission data, not a claim that source or work
// has been verified. The server must derive ActorID from an authenticated peer.
type CreateRunInput struct {
	Trigger        string      `json:"trigger"`
	ActorID        string      `json:"-"`
	Repository     string      `json:"repository"`
	Branch         string      `json:"branch"`
	CommitSHA      string      `json:"commit_sha"`
	SnapshotSHA256 string      `json:"snapshot_sha256"`
	CatalogSHA256  string      `json:"catalog_sha256"`
	ParentRunID    string      `json:"parent_run_id,omitempty"`
	Tasks          []TaskInput `json:"tasks"`
	IdempotencyKey string      `json:"-"`
	RequestSHA256  string      `json:"-"`
}

type TaskInput struct {
	Key             string          `json:"key"`
	Name            string          `json:"name"`
	Arguments       json.RawMessage `json:"args"`
	DependsOnKeys   []string        `json:"depends_on_keys"`
	Tier            string          `json:"tier"`
	Scope           string          `json:"scope"`
	HostClass       string          `json:"host_class"`
	DeadlineSeconds int             `json:"deadline_seconds"`
}

type Run struct {
	ID                string     `json:"id"`
	Trigger           string     `json:"trigger"`
	ActorID           string     `json:"actor_id"`
	Repository        string     `json:"repository"`
	Branch            string     `json:"branch"`
	CommitSHA         string     `json:"commit_sha"`
	SnapshotSHA256    string     `json:"snapshot_sha256"`
	CatalogSHA256     string     `json:"catalog_sha256"`
	ParentRunID       string     `json:"parent_run_id,omitempty"`
	State             string     `json:"state"`
	CancelRequestedAt *time.Time `json:"cancel_requested_at,omitempty"`
	CancelRequestedBy string     `json:"cancel_requested_by,omitempty"`
	ExecutionResult   string     `json:"execution_result,omitempty"`
	CleanupResult     string     `json:"cleanup_result"`
	EvidenceState     string     `json:"evidence_state"`
	CreatedAt         time.Time  `json:"created_at"`
	StartedAt         *time.Time `json:"started_at,omitempty"`
	EndedAt           *time.Time `json:"ended_at,omitempty"`
	Version           int64      `json:"version"`
	Tasks             []Task     `json:"tasks"`
}

type Task struct {
	ID              string          `json:"id"`
	RunID           string          `json:"run_id"`
	Key             string          `json:"key"`
	Name            string          `json:"name"`
	Arguments       json.RawMessage `json:"args"`
	Tier            string          `json:"tier"`
	Scope           string          `json:"scope"`
	HostClass       string          `json:"host_class"`
	State           string          `json:"state"`
	SkipReason      string          `json:"skip_reason,omitempty"`
	DeadlineSeconds int             `json:"deadline_seconds"`
	EnqueuedAt      time.Time       `json:"enqueued_at"`
	StartedAt       *time.Time      `json:"started_at,omitempty"`
	EndedAt         *time.Time      `json:"ended_at,omitempty"`
	Version         int64           `json:"version"`
	AttemptIDs      []string        `json:"attempt_ids"`
}

type StartAttemptInput struct {
	TaskID       string             `json:"task_id"`
	ActorID      string             `json:"actor_id"`
	ClaimedBy    string             `json:"-"`
	AgentID      string             `json:"agent_id,omitempty"`
	BoardLeaseID string             `json:"board_lease_id,omitempty"`
	Engine       string             `json:"engine"`
	Host         string             `json:"host"`
	HostCores    int                `json:"host_cores"`
	HostRAMBytes int64              `json:"host_ram_bytes"`
	HostLoad     float64            `json:"host_load"`
	HostFacts    json.RawMessage    `json:"host_facts"`
	HILTiming    *HILTimingEvidence `json:"-"`
}

// HILTimingEvidence is the server-derived, cohort-bound deadline decision
// persisted with a HIL attempt and returned unchanged on claim replay.
type HILTimingEvidence struct {
	Workload hilspec.Workload `json:"workload"`
	Decision hilspec.Decision `json:"decision"`
}

type Attempt struct {
	ID         string    `json:"id"`
	TaskID     string    `json:"task_id"`
	AttemptNo  int       `json:"attempt_no"`
	State      string    `json:"state"`
	StartedAt  time.Time `json:"started_at"`
	DeadlineAt time.Time `json:"deadline_at"`
}

type StepInput struct {
	AttemptID     string    `json:"attempt_id"`
	ActorID       string    `json:"actor_id"`
	Key           string    `json:"key"`
	Ordinal       int       `json:"ordinal"`
	Phase         string    `json:"phase"`
	StartedAt     time.Time `json:"started_at"`
	EndedAt       time.Time `json:"ended_at"`
	DurationNS    int64     `json:"duration_ns"`
	State         string    `json:"state"`
	ChildExitCode *int      `json:"child_exit_code,omitempty"`
}

type FinishAttemptInput struct {
	AttemptID        string `json:"attempt_id"`
	ActorID          string `json:"actor_id"`
	Result           string `json:"result"`
	ChildExitCode    *int   `json:"child_exit_code,omitempty"`
	HitDeadline      bool   `json:"hit_deadline"`
	EvidenceComplete bool   `json:"evidence_complete"`
	Reason           string `json:"reason,omitempty"`
}

// HILObservationInput binds a completed observation step to a trusted HIL cohort.
type HILObservationInput struct {
	AttemptID string
	ActorID   string
	StepKey   string
}
