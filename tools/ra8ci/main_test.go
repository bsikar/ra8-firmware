package main

import (
	"context"
	"strings"
	"testing"
)

func TestCommandSelectionRejectsUnknownAndUnsafeArguments(t *testing.T) {
	ctx := context.Background()
	if got := run(ctx, nil); got != 2 {
		t.Fatalf("empty command exit=%d", got)
	}
	if got := run(ctx, []string{"unknown-task"}); got != 2 {
		t.Fatalf("unknown task exit=%d", got)
	}
	if got := run(ctx, []string{"tasks", "extra"}); got != 2 {
		t.Fatalf("extra task list arg exit=%d", got)
	}
	if got := run(ctx, []string{"format-check", "--unreviewed"}); got != 2 {
		t.Fatalf("unreviewed arg exit=%d", got)
	}
	if got := run(ctx, []string{"db", "unknown"}); got != 2 {
		t.Fatalf("unknown db command exit=%d", got)
	}
	if got := run(ctx, []string{"server", "extra"}); got != 2 {
		t.Fatalf("server args exit=%d", got)
	}
	if got := run(ctx, []string{"agent", "extra"}); got != 2 {
		t.Fatalf("agent args exit=%d", got)
	}
	if got := run(ctx, []string{"sync", "extra"}); got != 2 {
		t.Fatalf("sync args exit=%d", got)
	}
	if got := run(ctx, []string{"run", "unknown"}); got != 1 {
		t.Fatalf("unknown run command exit=%d", got)
	}
	if err := runCommand(ctx, []string{"submit"}); err == nil {
		t.Fatal("run submission without an idempotency key or tasks was accepted")
	}
	if err := runCommand(ctx, []string{"status"}); err == nil {
		t.Fatal("run status without an ID was accepted")
	}
	for _, args := range [][]string{{"cancel"}, {"cancel", "not-an-id"}, {"cancel", "00000000-0000-7000-8000-000000000001", "extra"}} {
		if err := runCommand(ctx, args); err == nil || !strings.Contains(err.Error(), "usage") {
			t.Fatalf("run cancel accepted invalid args %v: %v", args, err)
		}
	}
	for _, args := range [][]string{{"logs"}, {"logs", "one-id"}, {"logs", "--limit", "9", "run", "attempt"}} {
		if err := runCommand(ctx, args); err == nil {
			t.Fatalf("run logs accepted invalid arguments %v", args)
		}
	}
	for _, args := range [][]string{{"logs"}, {"logs", "one-id"}, {"logs", "--limit", "9", "run", "attempt"}} {
		if err := runCommand(ctx, append([]string{"run"}, args...)); err == nil {
			t.Fatalf("run logs accepted invalid arguments %v", args)
		}
	}
	if got := run(ctx, []string{"board", "take", "ek-ra8d2"}); got != 1 {
		t.Fatalf("unsafe board command exit=%d", got)
	}
	if err := boardCommand(ctx, []string{"checkpoint", "bad/board"}); err == nil || !strings.Contains(err.Error(), "usage") {
		t.Fatalf("checkpoint accepted invalid board ID: %v", err)
	}
	if err := boardCommand(ctx, []string{"checkpoint", "ek-ra8d2"}); err == nil || !strings.Contains(err.Error(), "board client") {
		t.Fatalf("valid checkpoint skipped identity configuration: %v", err)
	}
	if got := run(ctx, []string{"tasks"}); got != 0 {
		t.Fatalf("list tasks exit=%d", got)
	}
}

func TestBoardCommandsValidateBeforeNetworkConfiguration(t *testing.T) {
	t.Setenv("RA8CI_SERVER_URL", "")
	t.Setenv("RA8CI_SERVER_CA", "")
	t.Setenv("RA8CI_CLIENT_CERT", "")
	t.Setenv("RA8CI_CLIENT_KEY", "")
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2"}); err == nil || !strings.Contains(err.Error(), "usage") {
		t.Fatalf("take without reason/duration accepted: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--why", "debug", "--duration", "30ms"}); err == nil || !strings.Contains(err.Error(), "whole number of seconds") {
		t.Fatalf("sub-second board lease accepted: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--class", "robot", "--why", "debug", "--duration", "30s"}); err == nil || !strings.Contains(err.Error(), "class must be human, ci, or agent") {
		t.Fatalf("unknown board priority class accepted: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--class", "agent", "--why", "debug", "--duration", "1h1m"}); err == nil || !strings.Contains(err.Error(), "between 1s and 1h0m0s") {
		t.Fatalf("agent board lease exceeded its one-hour cap: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--class", "ci", "--why", "build", "--duration", "2h1m"}); err == nil || !strings.Contains(err.Error(), "between 1s and 2h0m0s") {
		t.Fatalf("CI board lease exceeded its two-hour cap: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--class", "ci", "--why", "build", "--duration", "30s"}); err == nil || !strings.Contains(err.Error(), "board client") {
		t.Fatalf("valid CI board take skipped identity configuration: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--class", "agent", "--why", "debug", "--duration", "30s"}); err == nil || !strings.Contains(err.Error(), "board client") {
		t.Fatalf("valid agent board take skipped identity configuration: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"status", "ek-ra8d2"}); err == nil {
		t.Fatal("board status accepted without server identity")
	}
	if _, err := parseBoardCancel(nil); err == nil {
		t.Fatal("board cancel accepted no ticket")
	}
	if _, err := parseBoardCancel([]string{"ek-ra8d2", "not-a-request", "not-a-lease"}); err == nil {
		t.Fatal("board cancel accepted malformed ticket IDs")
	}
	ticket, err := parseBoardCancel([]string{"ek-ra8d2", "01996f90-3415-7cfe-8ff1-600058131afd", "01996f90-3415-7cfe-8ff1-600058131afe"})
	if err != nil || ticket.BoardID != "ek-ra8d2" || ticket.RequestID == "" || ticket.LeaseID == "" {
		t.Fatalf("valid board cancellation ticket rejected: %+v err=%v", ticket, err)
	}
	if err := boardCommand(context.Background(), []string{"cancel", "ek-ra8d2", "01996f90-3415-7cfe-8ff1-600058131afd", "01996f90-3415-7cfe-8ff1-600058131afe"}); err == nil || !strings.Contains(err.Error(), "board client") {
		t.Fatalf("valid cancel skipped identity configuration: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--why", "debug", "--duration", "30s"}); err == nil || !strings.Contains(err.Error(), "board client") {
		t.Fatalf("valid take skipped identity configuration: %v", err)
	}
}

func TestSyncFailsClosedWithoutClientIdentity(t *testing.T) {
	t.Setenv("RA8CI_SERVER_URL", "")
	t.Setenv("RA8CI_SERVER_CA", "")
	t.Setenv("RA8CI_CLIENT_CERT", "")
	t.Setenv("RA8CI_CLIENT_KEY", "")
	if err := syncLocalRuns(context.Background()); err == nil || !strings.Contains(err.Error(), "RA8CI_SERVER_URL") {
		t.Fatalf("missing sync identity accepted: %v", err)
	}
}

func TestAgentFailsClosedWithoutIdentity(t *testing.T) {
	t.Setenv("RA8CI_SERVER_URL", "")
	t.Setenv("RA8CI_SERVER_CA", "")
	t.Setenv("RA8CI_AGENT_CERT", "")
	t.Setenv("RA8CI_AGENT_KEY", "")
	t.Setenv("RA8CI_AGENT_ROOT", "")
	if err := runAgent(context.Background()); err == nil || !strings.Contains(err.Error(), "RA8CI_SERVER_URL") {
		t.Fatalf("missing agent identity accepted: %v", err)
	}
	t.Setenv("RA8CI_SERVER_URL", "https://control.example")
	t.Setenv("RA8CI_SERVER_CA", "/does/not/exist")
	t.Setenv("RA8CI_AGENT_CERT", "/does/not/exist")
	t.Setenv("RA8CI_AGENT_KEY", "/does/not/exist")
	t.Setenv("RA8CI_AGENT_ROOT", "/does/not/exist")
	t.Setenv("RA8CI_AGENT_POLL_WAIT", "26s")
	if err := runAgent(context.Background()); err == nil || !strings.Contains(err.Error(), "POLL_WAIT") {
		t.Fatalf("invalid agent polling accepted: %v", err)
	}
}

func TestLocalSourceIdentityDoesNotFabricateCommit(t *testing.T) {
	if identity, err := localSourceIdentity(context.Background(), t.TempDir()); err == nil || identity.CommitSHA != "" {
		t.Fatalf("non-git checkout received invented source identity: %+v %v", identity, err)
	}
}

func TestLocalTaskRequiresPrivateOutboxBeforeExecution(t *testing.T) {
	t.Setenv("RA8CI_STATE_DIR", "relative")
	if got := runLocalTask(context.Background(), []string{"format-check"}); got != 1 {
		t.Fatalf("unsafe outbox exit=%d", got)
	}
}

func TestServerAndMigrationFailClosedWithoutCredentials(t *testing.T) {
	t.Setenv("RA8CI_DATABASE_URL", "")
	t.Setenv("RA8CI_MIGRATION_DATABASE_URL", "")
	t.Setenv("RA8CI_TLS_CERT", "")
	if err := serve(context.Background()); err == nil || !strings.Contains(err.Error(), "RA8CI_DATABASE_URL") {
		t.Fatalf("server err=%v", err)
	}
	if err := migrate(context.Background()); err == nil {
		t.Fatal("migration accepted missing credential")
	}
}

func TestReportRejectsUnboundedAndUnauthenticatedRequests(t *testing.T) {
	t.Setenv("RA8CI_SERVER_URL", "")
	t.Setenv("RA8CI_SERVER_CA", "")
	t.Setenv("RA8CI_CLIENT_CERT", "")
	t.Setenv("RA8CI_CLIENT_KEY", "")
	ctx := context.Background()
	for _, args := range [][]string{
		{}, {"other"}, {"slow", "--window", "bad"},
		{"slow", "--window", "0s"}, {"slow", "--window", "9000h"},
		{"slow", "--limit", "0"}, {"slow", "--limit", "501"},
		{"slow", "unexpected"},
	} {
		if err := report(ctx, args); err == nil {
			t.Fatalf("accepted report args %v", args)
		}
	}
	if err := report(ctx, []string{"slow"}); err == nil || !strings.Contains(err.Error(), "RA8CI_SERVER_URL") {
		t.Fatalf("missing server identity accepted: %v", err)
	}
}

func TestGitHubCommandRequiresExplicitCheck(t *testing.T) {
	if err := githubCommand(context.Background(), nil); err == nil || !strings.Contains(err.Error(), "usage") {
		t.Fatalf("github command without check accepted: %v", err)
	}
}
