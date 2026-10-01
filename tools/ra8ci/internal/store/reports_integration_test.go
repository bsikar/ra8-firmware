//go:build integration

package store

import (
	"context"
	"errors"
	"testing"
	"time"
)

func TestIntegrationSlowTasksUsesCompletedEvidenceAndRepositoryScope(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	repo := "bsikar/report-" + mustID(t)
	in := testRun()
	in.Repository = repo
	in.Tasks = in.Tasks[:1]
	run, err := s.CreateRun(ctx, in)
	if err != nil {
		t.Fatal(err)
	}
	attempt, err := s.StartAttempt(ctx, testStart(run.Tasks[0].ID))
	if err != nil {
		t.Fatal(err)
	}
	zero := 0
	if err := s.FinishAttempt(ctx, FinishAttemptInput{AttemptID: attempt.ID, ActorID: "tester", Result: "succeeded", ChildExitCode: &zero, EvidenceComplete: true}); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO resource_samples
		(attempt_id,sample_no,monotonic_offset_ns,interval_ns,host_load,host_ram_available_bytes,host_os,host_load_kind)
		VALUES ($1,0,0,1000000000,2,4294967296,'linux','linux_load1')`, attempt.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO resource_samples
		(attempt_id,sample_no,monotonic_offset_ns,interval_ns,host_load,host_ram_available_bytes,host_os,host_load_kind)
		VALUES ($1,1,1000000000,1000000000,37.5,2147483648,'windows','cpu_busy_equivalent')`, attempt.ID); err != nil {
		t.Fatal(err)
	}
	rows, err := s.SlowTasks(ctx, repo, time.Now().Add(-time.Hour), 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 2 || rows[0].Name != "format-check" || rows[0].Tier != "required" ||
		rows[0].Samples != 1 || rows[0].MedianSeconds < 0 {
		t.Fatalf("unexpected report: %+v", rows)
	}
	var linux, windows *SlowTask
	for index := range rows {
		switch rows[index].HostOS {
		case "linux":
			linux = &rows[index]
		case "windows":
			windows = &rows[index]
		}
	}
	if linux == nil || linux.HostLoadKind != "linux_load1" || linux.ResourceSamples != 1 ||
		linux.MeanHostCores != 4 || linux.MeanLoadPerCore == nil || *linux.MeanLoadPerCore != 0.5 ||
		linux.MeanCPUBusyPct != nil || linux.MeanRAMUsedPct == nil || *linux.MeanRAMUsedPct != 50 {
		t.Fatalf("normalized Linux host context is wrong or misleading: %+v", linux)
	}
	if windows == nil || windows.HostLoadKind != "cpu_busy_equivalent" || windows.ResourceSamples != 1 ||
		windows.MeanCPUBusyPct == nil || *windows.MeanCPUBusyPct != 37.5 || windows.MeanLoadPerCore != nil ||
		windows.MeanRAMUsedPct == nil || *windows.MeanRAMUsedPct != 75 {
		t.Fatalf("Windows CPU-busy host context is wrong or mislabeled: %+v", windows)
	}
	foreign, err := s.SlowTasks(ctx, "someone/else", time.Now().Add(-time.Hour), 10)
	if err != nil || len(foreign) != 0 {
		t.Fatalf("foreign report: %+v, %v", foreign, err)
	}
	if _, err := s.SlowTasks(ctx, "", time.Now(), 10); !errors.Is(err, ErrInvalid) {
		t.Fatalf("empty repository: %v", err)
	}
	if _, err := s.SlowTasks(ctx, repo, time.Time{}, 10); !errors.Is(err, ErrInvalid) {
		t.Fatalf("missing window: %v", err)
	}
	if _, err := s.SlowTasks(ctx, repo, time.Now(), 501); !errors.Is(err, ErrInvalid) {
		t.Fatalf("unbounded page: %v", err)
	}
}
