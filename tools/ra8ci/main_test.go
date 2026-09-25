package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
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
	if err := boardCommand(ctx, []string{"checkpoint", "ek-ra8d2"}); err == nil || !strings.Contains(err.Error(), envClientCert) {
		t.Fatalf("valid checkpoint skipped identity configuration: %v", err)
	}
	if err := hilCommand(ctx, []string{"budget"}); err == nil || !strings.Contains(err.Error(), "usage") {
		t.Fatalf("HIL budget accepted missing required inputs: %v", err)
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
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--class", "ci", "--why", "build", "--duration", "30s"}); err == nil || !strings.Contains(err.Error(), envClientCert) {
		t.Fatalf("valid CI board take skipped identity configuration: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--class", "agent", "--why", "debug", "--duration", "30s"}); err == nil || !strings.Contains(err.Error(), envClientCert) {
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
	if err := boardCommand(context.Background(), []string{"cancel", "ek-ra8d2", "01996f90-3415-7cfe-8ff1-600058131afd", "01996f90-3415-7cfe-8ff1-600058131afe"}); err == nil || !strings.Contains(err.Error(), envClientCert) {
		t.Fatalf("valid cancel skipped identity configuration: %v", err)
	}
	if err := boardCommand(context.Background(), []string{"take", "ek-ra8d2", "--why", "debug", "--duration", "30s"}); err == nil || !strings.Contains(err.Error(), envClientCert) {
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

// firstCatalogTask returns a real reviewed task name. The correspondence is
// checked against the catalog at startup, so a test that invented a name would
// only ever exercise the refusal path.
func firstCatalogTask(t *testing.T) string {
	t.Helper()
	loaded, err := catalog.Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	names := loaded.Names()
	if len(names) == 0 {
		t.Fatal("catalog carries no tasks")
	}
	return names[0]
}

func writeCorrespondence(t *testing.T, pairs string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "correspondence.json")
	if err := os.WriteFile(path, []byte(pairs), 0o600); err != nil {
		t.Fatalf("write correspondence: %v", err)
	}
	return path
}

func TestGitHubSubcommandsAreDispatchedByName(t *testing.T) {
	ctx := context.Background()
	for _, args := range [][]string{nil, {}, {"shadow", "extra"}, {"publish"}, {""}} {
		err := githubCommand(ctx, args)
		if err == nil || !strings.Contains(err.Error(), "usage") {
			t.Fatalf("github %v accepted: %v", args, err)
		}
	}
}

// An unknown subcommand is refused on its name alone. It must not depend on
// the environment being configured, or a deployment would learn about a typo
// only where the configuration happens to be present.
func TestUnknownGitHubSubcommandIsRefusedBeforeReadingTheEnvironment(t *testing.T) {
	t.Setenv(github.EnvCheckRunMode, "authoritative")
	t.Setenv(github.EnvShadowCorrespondenceFile, filepath.Join(t.TempDir(), "absent.json"))
	err := githubCommand(context.Background(), []string{"publish"})
	if err == nil || !strings.Contains(err.Error(), "usage") {
		t.Fatalf("unknown subcommand accepted: %v", err)
	}
}

func TestShadowConfigRefusesWhenCheckRunPublishingIsNotConfigured(t *testing.T) {
	t.Setenv(github.EnvCheckRunMode, "")
	os.Unsetenv(github.EnvCheckRunMode)
	t.Setenv(github.EnvShadowCorrespondenceFile, "")
	os.Unsetenv(github.EnvShadowCorrespondenceFile)

	var out bytes.Buffer
	err := githubShadowConfig(&out)
	if err == nil || !strings.Contains(err.Error(), "not configured") {
		t.Fatalf("unconfigured shadow report accepted: %v", err)
	}
	if !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
		t.Fatalf("refusal does not name the variable to set: %v", err)
	}
	if out.Len() != 0 {
		t.Fatalf("refused report wrote output: %q", out.String())
	}
}

// A declaration this build would refuse is refused by the command too, with
// nothing written. The report exists to be read as configuration that works.
func TestShadowConfigRefusalsLeaveTheStreamUntouched(t *testing.T) {
	task := firstCatalogTask(t)
	cases := map[string]struct {
		mode  string
		pairs string
	}{
		"unknown mode":   {mode: "required", pairs: `{"` + task + `":"build"}`},
		"unknown task":   {mode: "shadow", pairs: `{"no-such-reviewed-task":"build"}`},
		"repeated task":  {mode: "shadow", pairs: `{"` + task + `":"build","` + task + `":"test"}`},
		"not an object":  {mode: "shadow", pairs: `["` + task + `"]`},
		"shared job":     {mode: "shadow", pairs: `{"` + task + `":"build","` + task + `":"build"}`},
		"empty document": {mode: "shadow", pairs: `{}`},
	}
	for name, testCase := range cases {
		t.Run(name, func(t *testing.T) {
			t.Setenv(github.EnvCheckRunMode, testCase.mode)
			t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t, testCase.pairs))
			var out bytes.Buffer
			if err := githubShadowConfig(&out); err == nil {
				t.Fatal("invalid configuration accepted")
			}
			if out.Len() != 0 {
				t.Fatalf("refused report wrote output: %q", out.String())
			}
		})
	}
}

// An absent mode is shadow at the command, not only inside the loader. This is
// the line an operator reads to answer "can this deployment block a merge".
func TestShadowConfigReportsShadowWhenTheModeIsUnset(t *testing.T) {
	task := firstCatalogTask(t)
	t.Setenv(github.EnvCheckRunMode, "")
	os.Unsetenv(github.EnvCheckRunMode)
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t, `{"`+task+`":"build"}`))

	var out bytes.Buffer
	if err := githubShadowConfig(&out); err != nil {
		t.Fatalf("shadow report refused: %v", err)
	}
	var report struct {
		Mode           string              `json:"mode"`
		MayBlockMerges bool                `json:"may_block_merges"`
		CatalogTasks   int                 `json:"catalog_tasks"`
		CoveredTasks   int                 `json:"covered_tasks"`
		Correspondence []map[string]string `json:"correspondence"`
		Uncovered      []string            `json:"uncovered_tasks"`
		Digest         string              `json:"catalog_digest"`
	}
	if err := json.Unmarshal(out.Bytes(), &report); err != nil {
		t.Fatalf("report is not JSON: %v: %q", err, out.String())
	}
	if report.Mode != "shadow" || report.MayBlockMerges {
		t.Fatalf("absent mode did not report a non-blocking shadow deployment: %+v", report)
	}
	if report.CoveredTasks != 1 || len(report.Correspondence) != 1 {
		t.Fatalf("one declared pair did not report as one: %+v", report)
	}
	if report.Correspondence[0]["task"] != task || report.Correspondence[0]["actions_job"] != "build" {
		t.Fatalf("declared pair not reported: %+v", report.Correspondence)
	}
	if report.Digest == "" {
		t.Fatal("report does not name the catalog it was checked against")
	}
	if report.CatalogTasks <= report.CoveredTasks {
		t.Fatalf("catalog reported as no larger than the declaration: %+v", report)
	}
	// Every task outside the declaration is named, because a plane
	// outcome the correspondence does not cover is refused, not dropped.
	if len(report.Uncovered) != report.CatalogTasks-report.CoveredTasks {
		t.Fatalf("uncovered tasks not fully named: %d of %d: %+v",
			len(report.Uncovered), report.CatalogTasks-report.CoveredTasks, report)
	}
	for _, name := range report.Uncovered {
		if name == task {
			t.Fatal("a covered task was reported as uncovered")
		}
	}
	if !sortedStrings(report.Uncovered) {
		t.Fatalf("uncovered tasks are not in a diffable order: %v", report.Uncovered)
	}
}

// An authoritative deployment says so in the same field, so the two are read
// from one place rather than inferred from the namespace.
func TestShadowConfigReportsAnAuthoritativeDeploymentAsBlocking(t *testing.T) {
	task := firstCatalogTask(t)
	t.Setenv(github.EnvCheckRunMode, "authoritative")
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t, `{"`+task+`":"build"}`))

	var out bytes.Buffer
	if err := githubShadowConfig(&out); err != nil {
		t.Fatalf("shadow report refused: %v", err)
	}
	var report struct {
		Mode           string `json:"mode"`
		MayBlockMerges bool   `json:"may_block_merges"`
	}
	if err := json.Unmarshal(out.Bytes(), &report); err != nil {
		t.Fatalf("report is not JSON: %v", err)
	}
	if report.Mode != "authoritative" || !report.MayBlockMerges {
		t.Fatalf("authoritative deployment not reported as blocking: %+v", report)
	}
}

// The report speaks to nobody: it is configuration, not a publish. Pointing
// the scale-set variables at an address nothing listens on must not change the
// outcome, because nothing here opens a connection.
func TestShadowConfigPublishesNothing(t *testing.T) {
	task := firstCatalogTask(t)
	t.Setenv(github.EnvConfigURL, "https://127.0.0.1:1/")
	t.Setenv(github.EnvOwner, "bsikar")
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t, `{"`+task+`":"build"}`))

	var out bytes.Buffer
	if err := githubShadowConfig(&out); err != nil {
		t.Fatalf("shadow report refused: %v", err)
	}
	if !strings.Contains(out.String(), task) {
		t.Fatalf("report does not carry the declaration: %q", out.String())
	}
}

func sortedStrings(values []string) bool {
	for i := 1; i < len(values); i++ {
		if values[i-1] > values[i] {
			return false
		}
	}
	return true
}

// shadowCompareEnv configures a correspondence covering one real task and
// returns that task name.
func shadowCompareEnv(t *testing.T, job string) string {
	t.Helper()
	task := firstCatalogTask(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t, `{"`+task+`":"`+job+`"}`))
	return task
}

const shadowCompareHead = "0123456789abcdef0123456789abcdef01234567"

func TestShadowCompareRendersAndPassesOnAgreement(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	input := `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"}]}`

	var out bytes.Buffer
	if err := githubShadowCompare(strings.NewReader(input), &out); err != nil {
		t.Fatalf("agreeing comparison refused: %v", err)
	}
	page := out.String()
	if !strings.Contains(page, shadowCompareHead) || !strings.Contains(page, task) {
		t.Fatalf("page does not carry the commit and the task: %q", page)
	}
}

// A report that is not clean still renders, and the refusal comes after the
// page. A caller reading only the exit status must not be able to get a clean
// one from a report nobody could read.
func TestShadowCompareRendersBeforeReportingAnUncleanVerdict(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	cases := map[string]string{
		"conflicting":   `"success"`,
		"indeterminate": `""`,
	}
	for name, conclusion := range cases {
		t.Run(name, func(t *testing.T) {
			input := `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"failure"}],` +
				`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":` + conclusion + `}]}`
			var out bytes.Buffer
			err := githubShadowCompare(strings.NewReader(input), &out)
			if err == nil || !strings.Contains(err.Error(), "not clean") {
				t.Fatalf("unclean comparison reported as clean: %v", err)
			}
			if !strings.Contains(out.String(), shadowCompareHead) {
				t.Fatalf("unclean comparison rendered nothing: %q", out.String())
			}
		})
	}
}

// A task the plane did not report is named after the page, never as a pairing.
// A selection that skipped it is a normal commit, not a gap in the evidence.
func TestShadowCompareNamesTheTasksThisCommitDidNotExercise(t *testing.T) {
	task := firstCatalogTask(t)
	loaded, err := catalog.Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	names := loaded.Names()
	if len(names) < 2 {
		t.Skip("catalog carries fewer than two tasks")
	}
	other := names[1]
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile,
		writeCorrespondence(t, `{"`+task+`":"build","`+other+`":"test"}`))

	input := `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"}]}`
	var out bytes.Buffer
	if err := githubShadowCompare(strings.NewReader(input), &out); err != nil {
		t.Fatalf("comparison refused: %v", err)
	}
	page := out.String()
	if !strings.Contains(page, "not exercised on this commit: "+other) {
		t.Fatalf("unexercised task not named: %q", page)
	}
	if strings.Count(page, other) != 1 {
		t.Fatalf("unexercised task appears as a pairing: %q", page)
	}
}

func TestShadowCompareRefusesInputItCannotReadAsOneCommit(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	pair := `{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}`
	other := "89abcdef0123456789abcdef0123456789abcdef"
	cases := map[string]string{
		"not an object":      `[` + pair + `]`,
		"unknown field":      `{"plane":[` + pair + `],"runs":[]}`,
		"trailing document":  `{"plane":[` + pair + `]}{"plane":[]}`,
		"no plane outcomes":  `{"plane":[],"actions":[]}`,
		"uncovered task":     `{"plane":[{"task":"no-such-reviewed-task","head_sha":"` + shadowCompareHead + `","observed":"success"}]}`,
		"two commits":        `{"plane":[` + pair + `],"actions":[{"job":"build","head_sha":"` + other + `","conclusion":"success"}]}`,
		"unknown conclusion": `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"melted"}]}`,
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			var out bytes.Buffer
			if err := githubShadowCompare(strings.NewReader(input), &out); err == nil {
				t.Fatalf("unreadable input accepted, wrote %q", out.String())
			}
		})
	}
}

func TestShadowCompareRefusesWhenCheckRunPublishingIsNotConfigured(t *testing.T) {
	t.Setenv(github.EnvCheckRunMode, "")
	os.Unsetenv(github.EnvCheckRunMode)
	t.Setenv(github.EnvShadowCorrespondenceFile, "")
	os.Unsetenv(github.EnvShadowCorrespondenceFile)

	var out bytes.Buffer
	err := githubShadowCompare(strings.NewReader(`{"plane":[]}`), &out)
	if err == nil || !strings.Contains(err.Error(), "not configured") {
		t.Fatalf("unconfigured comparison accepted: %v", err)
	}
	if out.Len() != 0 {
		t.Fatalf("refused comparison wrote output: %q", out.String())
	}
}

// The comparison is graded on what the plane observed, never on the mode it
// publishes under: an authoritative deployment grades the same evidence.
func TestShadowCompareGradesTheSameEvidenceInEitherMode(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	input := `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"}]}`
	var shadow bytes.Buffer
	if err := githubShadowCompare(strings.NewReader(input), &shadow); err != nil {
		t.Fatalf("shadow comparison refused: %v", err)
	}
	t.Setenv(github.EnvCheckRunMode, "authoritative")
	var authoritative bytes.Buffer
	if err := githubShadowCompare(strings.NewReader(input), &authoritative); err != nil {
		t.Fatalf("authoritative comparison refused: %v", err)
	}
	if shadow.String() != authoritative.String() {
		t.Fatalf("mode changed the graded page:\n%q\n%q", shadow.String(), authoritative.String())
	}
}

// publishCheckRunEnv configures a complete publishing environment covering one
// real catalog task, and returns that task name. The private key file does not
// exist: every test using this stops before a publisher is built, which is
// itself the pin that refusals come first.
func publishCheckRunEnv(t *testing.T) string {
	t.Helper()
	task := shadowCompareEnv(t, "build")
	t.Setenv(github.EnvCheckRunRepository, "ra8-firmware")
	t.Setenv(github.EnvAppClientID, "Iv1.0123456789abcdef")
	t.Setenv(github.EnvInstallationID, "94213")
	t.Setenv(github.EnvPrivateKeyFile, filepath.Join(t.TempDir(), "absent.pem"))
	t.Setenv(github.EnvOwner, "bsikar")
	return task
}

// planCheckRunsInput builds a one-task document for the planner.
func planCheckRunsInput(task, state, summary string) checkRunPublishInput {
	document := checkRunPublishInput{HeadSHA: shadowCompareHead}
	document.Runs = append(document.Runs, struct {
		Task    string `json:"task"`
		State   string `json:"state"`
		Summary string `json:"summary"`
	}{Task: task, State: state, Summary: summary})
	return document
}

func TestPublishCheckRunNeedsBothHalvesOfTheConfiguration(t *testing.T) {
	task := firstCatalogTask(t)
	t.Run("no correspondence", func(t *testing.T) {
		for _, name := range []string{github.EnvShadowCorrespondenceFile, github.EnvCheckRunMode, github.EnvCheckRunRepository} {
			t.Setenv(name, "")
			os.Unsetenv(name)
		}
		err := githubPublishCheckRuns(context.Background(), strings.NewReader("{}"), io.Discard)
		if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
			t.Fatalf("error %v, want one naming %s", err, github.EnvShadowCorrespondenceFile)
		}
	})
	t.Run("no repository", func(t *testing.T) {
		t.Setenv(github.EnvCheckRunMode, "shadow")
		t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t, `{"`+task+`":"build"}`))
		t.Setenv(github.EnvCheckRunRepository, "")
		os.Unsetenv(github.EnvCheckRunRepository)

		err := githubPublishCheckRuns(context.Background(), strings.NewReader("{}"), io.Discard)
		if err == nil || !strings.Contains(err.Error(), github.EnvCheckRunRepository) {
			t.Fatalf("error %v, want one naming %s", err, github.EnvCheckRunRepository)
		}
	})
}

// A shadow deployment posts runs that cannot hold a pull request, and the
// observed conclusion still travels so the comparison can be made.
func TestPlannedRunsCarryTheDeploymentsMode(t *testing.T) {
	task := firstCatalogTask(t)
	correspondence, err := github.NewShadowCorrespondence(map[string]string{task: "build"}, []string{task})
	if err != nil {
		t.Fatalf("correspondence: %v", err)
	}
	document := planCheckRunsInput(task, "failed", "")

	shadow, err := planCheckRuns(github.ModeShadow, correspondence, document)
	if err != nil {
		t.Fatalf("plan shadow: %v", err)
	}
	if shadow[0].Run.Blocking() {
		t.Fatalf("a shadow run would hold a pull request: %+v", shadow[0].Run)
	}
	if shadow[0].Run.Observed != "failure" {
		t.Fatalf("observed %q, want failure", shadow[0].Run.Observed)
	}

	authoritative, err := planCheckRuns(github.ModeAuthoritative, correspondence, document)
	if err != nil {
		t.Fatalf("plan authoritative: %v", err)
	}
	if !authoritative[0].Run.Blocking() {
		t.Fatalf("an authoritative failure would not hold a pull request: %+v", authoritative[0].Run)
	}
	if shadow[0].Run.Name == authoritative[0].Run.Name {
		t.Fatalf("both modes published under one name: %q", shadow[0].Run.Name)
	}
}

// A blank summary publishes a check run a reviewer cannot act on, and a
// summary the caller wrote is theirs.
func TestPlannedSummaryFallsBackToTheFactsTheRunCarries(t *testing.T) {
	task := firstCatalogTask(t)
	correspondence, err := github.NewShadowCorrespondence(map[string]string{task: "build"}, []string{task})
	if err != nil {
		t.Fatalf("correspondence: %v", err)
	}

	planned, err := planCheckRuns(github.ModeShadow, correspondence, planCheckRunsInput(task, "succeeded", "  "))
	if err != nil {
		t.Fatalf("plan: %v", err)
	}
	for _, want := range []string{task, "success", shadowCompareHead} {
		if !strings.Contains(planned[0].Summary, want) {
			t.Fatalf("composed summary %q does not carry %q", planned[0].Summary, want)
		}
	}

	planned, err = planCheckRuns(github.ModeShadow, correspondence, planCheckRunsInput(task, "succeeded", "ran on the bench"))
	if err != nil {
		t.Fatalf("plan: %v", err)
	}
	if planned[0].Summary != "ran on the bench" {
		t.Fatalf("summary %q, want the one the caller wrote", planned[0].Summary)
	}
}

// A check run cannot be taken back once GitHub has it, so a document with a
// bad outcome anywhere in it is refused whole.
func TestPlanRefusesTheWholeDocumentRatherThanPostingPartOfIt(t *testing.T) {
	task := firstCatalogTask(t)
	correspondence, err := github.NewShadowCorrespondence(map[string]string{task: "build"}, []string{task})
	if err != nil {
		t.Fatalf("correspondence: %v", err)
	}
	good := planCheckRunsInput(task, "succeeded", "").Runs[0]

	cases := map[string]checkRunPublishInput{
		"no outcomes":    {HeadSHA: shadowCompareHead},
		"uncovered task": planCheckRunsInput("a-task-no-correspondence-covers", "succeeded", ""),
		"unknown state":  planCheckRunsInput(task, "exploded", ""),
		"unfinished":     planCheckRunsInput(task, "running", ""),
		"short sha": {HeadSHA: "0123456", Runs: []struct {
			Task    string `json:"task"`
			State   string `json:"state"`
			Summary string `json:"summary"`
		}{good}},
		"task twice": {HeadSHA: shadowCompareHead, Runs: []struct {
			Task    string `json:"task"`
			State   string `json:"state"`
			Summary string `json:"summary"`
		}{good, good}},
	}
	for name, document := range cases {
		t.Run(name, func(t *testing.T) {
			planned, err := planCheckRuns(github.ModeShadow, correspondence, document)
			if err == nil {
				t.Fatalf("accepted %d runs", len(planned))
			}
			if planned != nil {
				t.Fatalf("a refused plan returned %d runs", len(planned))
			}
		})
	}
}

// The document is read and planned before a publisher exists, so a bad
// document is reported as a bad document rather than as a credential problem,
// and nothing reaches GitHub.
func TestPublishCheckRunRefusesTheDocumentBeforeBuildingAPublisher(t *testing.T) {
	task := publishCheckRunEnv(t)
	cases := map[string]string{
		"not an object":     `[]`,
		"unknown field":     `{"head_sha":"` + shadowCompareHead + `","commit":"x"}`,
		"trailing document": `{"head_sha":"` + shadowCompareHead + `","runs":[{"task":"` + task + `","state":"succeeded"}]} {}`,
		"no runs":           `{"head_sha":"` + shadowCompareHead + `"}`,
		"uncovered task":    `{"head_sha":"` + shadowCompareHead + `","runs":[{"task":"not-a-covered-task","state":"succeeded"}]}`,
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			var out bytes.Buffer
			err := githubPublishCheckRuns(context.Background(), strings.NewReader(input), &out)
			if err == nil {
				t.Fatal("a document that cannot be published was accepted")
			}
			if strings.Contains(err.Error(), "absent.pem") {
				t.Fatalf("the key was opened before the document was judged: %v", err)
			}
			if out.Len() != 0 {
				t.Fatalf("a refused publish wrote %q", out.String())
			}
		})
	}
}

// requiredChecksEnv declares a correspondence covering one catalog task and
// returns that task, so the gate plan has something to plan for.
func requiredChecksEnv(t *testing.T, mode string) string {
	t.Helper()
	task := firstCatalogTask(t)
	t.Setenv(github.EnvCheckRunMode, mode)
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t, `{"`+task+`":"build"}`))
	return task
}

func decodeRequiredChecks(t *testing.T, out *bytes.Buffer) map[string]any {
	t.Helper()
	var report map[string]any
	if err := json.Unmarshal(out.Bytes(), &report); err != nil {
		t.Fatalf("report is not JSON: %v (%q)", err, out.String())
	}
	return report
}

func requiredCheckList(t *testing.T, report map[string]any, field string) []string {
	t.Helper()
	raw, present := report[field]
	if !present {
		t.Fatalf("report has no %q section", field)
	}
	values, ok := raw.([]any)
	if !ok {
		t.Fatalf("%q is %T, want a list", field, raw)
	}
	out := make([]string, 0, len(values))
	for _, value := range values {
		text, ok := value.(string)
		if !ok {
			t.Fatalf("%q carries %T, want a string", field, value)
		}
		out = append(out, text)
	}
	return out
}

func TestRequiredChecksPlansTheGateForTheCoveredTasks(t *testing.T) {
	task := requiredChecksEnv(t, "authoritative")
	var out bytes.Buffer
	if err := githubRequiredChecks(strings.NewReader(`{"required":[]}`), &out); err != nil {
		t.Fatalf("plan refused: %v", err)
	}
	report := decodeRequiredChecks(t, &out)
	add := requiredCheckList(t, report, "add")
	want := "ra8ci / " + task
	if len(add) != 1 || add[0] != want {
		t.Fatalf("add = %v, want [%s]", add, want)
	}
	if report["may_block_merges"] != true {
		t.Fatalf("may_block_merges = %v for an authoritative deployment", report["may_block_merges"])
	}
	if report["no_change"] != false {
		t.Fatalf("no_change = %v for a plan that adds a context", report["no_change"])
	}
}

// A shadow deployment plans no additions: a shadow run reports neutral whatever
// the task did, so a required check pointed at one is satisfied by a task that
// failed.
func TestRequiredChecksAddsNothingInShadowMode(t *testing.T) {
	requiredChecksEnv(t, "shadow")
	var out bytes.Buffer
	if err := githubRequiredChecks(strings.NewReader(`{"required":[]}`), &out); err != nil {
		t.Fatalf("plan refused: %v", err)
	}
	report := decodeRequiredChecks(t, &out)
	if add := requiredCheckList(t, report, "add"); len(add) != 0 {
		t.Fatalf("shadow plan adds %v", add)
	}
	if report["may_block_merges"] != false {
		t.Fatalf("may_block_merges = %v for a shadow deployment", report["may_block_merges"])
	}
	if report["no_change"] != true {
		t.Fatalf("no_change = %v for a shadow plan over an empty gate", report["no_change"])
	}
}

// The gate carries contexts this plane does not own. They are reported so the
// operator sees the whole list, and never proposed for removal.
func TestRequiredChecksLeavesForeignContextsAlone(t *testing.T) {
	requiredChecksEnv(t, "authoritative")
	var out bytes.Buffer
	input := `{"required":["CodeQL","build (ubuntu-latest)"]}`
	if err := githubRequiredChecks(strings.NewReader(input), &out); err != nil {
		t.Fatalf("plan refused: %v", err)
	}
	report := decodeRequiredChecks(t, &out)
	foreign := requiredCheckList(t, report, "foreign")
	if len(foreign) != 2 || foreign[0] != "CodeQL" || foreign[1] != "build (ubuntu-latest)" {
		t.Fatalf("foreign = %v", foreign)
	}
	if remove := requiredCheckList(t, report, "remove"); len(remove) != 0 {
		t.Fatalf("plan proposes removing %v, which it does not own", remove)
	}
}

// An empty section renders as [] rather than null, so two plans diff against
// each other instead of one of them missing a field.
func TestRequiredChecksRendersEmptySectionsAsLists(t *testing.T) {
	requiredChecksEnv(t, "authoritative")
	var out bytes.Buffer
	if err := githubRequiredChecks(strings.NewReader(`{"required":[]}`), &out); err != nil {
		t.Fatalf("plan refused: %v", err)
	}
	if strings.Contains(out.String(), "null") {
		t.Fatalf("report carries a null section: %q", out.String())
	}
	for _, field := range []string{"add", "remove", "keep", "foreign"} {
		requiredCheckList(t, decodeRequiredChecks(t, &out), field)
	}
}

func TestRequiredChecksRefusesInputItCannotRead(t *testing.T) {
	cases := map[string]string{
		"not an object":      `["ra8ci / build"]`,
		"unknown field":      `{"required":[],"contexts":[]}`,
		"trailing document":  `{"required":[]}{"required":[]}`,
		"padded context":     `{"required":[" CodeQL "]}`,
		"same context twice": `{"required":["CodeQL","CodeQL"]}`,
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			requiredChecksEnv(t, "authoritative")
			var out bytes.Buffer
			if err := githubRequiredChecks(strings.NewReader(input), &out); err == nil {
				t.Fatal("unreadable input accepted")
			}
			if out.Len() != 0 {
				t.Fatalf("a refused plan wrote %q", out.String())
			}
		})
	}
}

func TestRequiredChecksRefusesWhenCheckRunPublishingIsNotConfigured(t *testing.T) {
	t.Setenv(github.EnvCheckRunMode, "")
	os.Unsetenv(github.EnvCheckRunMode)
	t.Setenv(github.EnvShadowCorrespondenceFile, "")
	os.Unsetenv(github.EnvShadowCorrespondenceFile)

	var out bytes.Buffer
	if err := githubRequiredChecks(strings.NewReader(`{"required":[]}`), &out); err == nil {
		t.Fatal("planned a gate with nothing configured")
	}
	if out.Len() != 0 {
		t.Fatalf("a refused plan wrote %q", out.String())
	}
}

// The subcommand is dispatched by name, and an unknown one is refused before
// the environment is read.
func TestRequiredChecksIsDispatchedByName(t *testing.T) {
	if err := githubCommand(context.Background(), []string{"required-check"}); err == nil {
		t.Fatal("a misspelled subcommand was accepted")
	} else if !strings.Contains(err.Error(), "required-checks") {
		t.Fatalf("usage does not name the subcommand: %v", err)
	}
}
