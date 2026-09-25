package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"unicode/utf8"

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

// twoCatalogTasks returns two real reviewed task names, for a test that needs
// one the document plans and one it does not.
func twoCatalogTasks(t *testing.T) (string, string) {
	t.Helper()
	loaded, err := catalog.Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	names := loaded.Names()
	if len(names) < 2 {
		t.Skip("catalog carries fewer than two tasks")
	}
	return names[0], names[1]
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

// gateEnv configures a deployment that could read a gate, with a private key
// file that does not exist: every test below is refused before a reader is
// built, so none of them speaks to GitHub.
func gateEnv(t *testing.T) {
	t.Helper()
	publishCheckRunEnv(t)
}

// The document `gate` writes is the document `required-checks` reads, with no
// extra field. required-checks refuses a field it does not know, so an echoed
// branch name would break the pipe this command exists for.
func TestTheGateDocumentIsExactlyWhatRequiredChecksReads(t *testing.T) {
	var gateOutput bytes.Buffer
	encoder := json.NewEncoder(&gateOutput)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(requiredCheckInput{Required: emptyWhenNil([]string{"CodeQL"})}); err != nil {
		t.Fatalf("encode gate document: %v", err)
	}
	if strings.Contains(gateOutput.String(), "branch") {
		t.Fatalf("the gate document carries a field required-checks does not read: %q", gateOutput.String())
	}

	requiredChecksEnv(t, "authoritative")
	var plan bytes.Buffer
	if err := githubRequiredChecks(strings.NewReader(gateOutput.String()), &plan); err != nil {
		t.Fatalf("required-checks refused the gate document: %v", err)
	}
	if !strings.Contains(plan.String(), "CodeQL") {
		t.Fatalf("the planned gate lost the context it was given: %q", plan.String())
	}
}

// The branch is named, never defaulted: a default would read some other
// branch's protection and report it as the gate.
func TestGateRefusesADocumentThatNamesNoBranch(t *testing.T) {
	for name, input := range map[string]string{
		"empty object":      `{}`,
		"empty branch":      `{"branch":""}`,
		"not an object":     `["main"]`,
		"unknown field":     `{"ref":"main"}`,
		"trailing document": `{"branch":"main"}{"branch":"dev"}`,
	} {
		t.Run(name, func(t *testing.T) {
			gateEnv(t)
			var out bytes.Buffer
			if err := githubGate(context.Background(), strings.NewReader(input), &out); err == nil {
				t.Fatal("document was accepted")
			}
			if out.Len() != 0 {
				t.Fatalf("a refused read wrote %q", out.String())
			}
		})
	}
}

// A deployment that cannot say which repository's gate it means is refused
// before anything is read, naming the variable that is missing.
func TestGateNeedsBothHalvesOfTheConfiguration(t *testing.T) {
	t.Run("no correspondence", func(t *testing.T) {
		var out bytes.Buffer
		err := githubGate(context.Background(), strings.NewReader(`{"branch":"main"}`), &out)
		if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
			t.Fatalf("returned %v, want the correspondence variable named", err)
		}
		if out.Len() != 0 {
			t.Fatalf("a refused read wrote %q", out.String())
		}
	})
	t.Run("no repository", func(t *testing.T) {
		shadowCompareEnv(t, "build")
		var out bytes.Buffer
		err := githubGate(context.Background(), strings.NewReader(`{"branch":"main"}`), &out)
		if err == nil || !strings.Contains(err.Error(), github.EnvCheckRunRepository) {
			t.Fatalf("returned %v, want the repository variable named", err)
		}
		if out.Len() != 0 {
			t.Fatalf("a refused read wrote %q", out.String())
		}
	})
}

// The document is read before a reader exists, so a bad document is reported
// as a bad document rather than as a credential problem. The key file these
// tests name does not exist, so reaching the reader is itself the failure.
func TestGateReadsTheDocumentBeforeBuildingAReader(t *testing.T) {
	gateEnv(t)
	var out bytes.Buffer
	err := githubGate(context.Background(), strings.NewReader(`{"branch":"main"}`), &out)
	if err == nil {
		t.Fatal("a reader was built from a key file that does not exist")
	}
	if strings.Contains(err.Error(), "read the branch to report") {
		t.Fatalf("a readable document was reported as a document problem: %v", err)
	}
	if out.Len() != 0 {
		t.Fatalf("a refused read wrote %q", out.String())
	}
}

// gate is dispatched by name like every other subcommand, and a near miss is
// refused with a usage line naming the real one.
func TestGateIsDispatchedByName(t *testing.T) {
	err := githubCommand(context.Background(), []string{"gates"})
	if err == nil || !strings.Contains(err.Error(), "gate") {
		t.Fatalf("github gates returned %v", err)
	}
}

// shadowEvidenceCommit builds one pull request's comparison document for the
// evidence command, using a real catalog task name.
func shadowEvidenceCommit(task, head, observed, conclusion string) string {
	return `{"plane":[{"task":"` + task + `","head_sha":"` + head + `","observed":"` + observed + `"}],` +
		`"actions":[{"job":"build","head_sha":"` + head + `","conclusion":"` + conclusion + `"}]}`
}

func shadowEvidenceInputDocument(threshold int, commits ...string) string {
	return `{"threshold":` + strconv.Itoa(threshold) + `,"commits":[` + strings.Join(commits, ",") + `]}`
}

func decodeShadowEvidence(t *testing.T, raw string) map[string]any {
	t.Helper()
	var report map[string]any
	if err := json.Unmarshal([]byte(raw), &report); err != nil {
		t.Fatalf("decode evidence report: %v\n%s", err, raw)
	}
	return report
}

const (
	evidenceHeadOne   = "1111111111111111111111111111111111111111"
	evidenceHeadTwo   = "2222222222222222222222222222222222222222"
	evidenceHeadThree = "3333333333333333333333333333333333333333"
)

// The command exists because #1481's hold condition is plural: one clean pull
// request is not the comparison "over representative pull requests" the issue
// asks for.
func TestShadowEvidenceSettlesOnlyWhenTheThresholdIsMet(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	for _, testCase := range []struct {
		name      string
		threshold int
		commits   []string
		settled   bool
	}{
		{"two graded commits at threshold two", 2, []string{
			shadowEvidenceCommit(task, evidenceHeadOne, "success", "success"),
			shadowEvidenceCommit(task, evidenceHeadTwo, "success", "success"),
		}, true},
		{"two graded commits at threshold three", 3, []string{
			shadowEvidenceCommit(task, evidenceHeadOne, "success", "success"),
			shadowEvidenceCommit(task, evidenceHeadTwo, "success", "success"),
		}, false},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			var out bytes.Buffer
			err := githubShadowEvidence(
				strings.NewReader(shadowEvidenceInputDocument(testCase.threshold, testCase.commits...)), &out)
			report := decodeShadowEvidence(t, out.String())
			if report["settled"] != testCase.settled {
				t.Fatalf("settled %v, want %v", report["settled"], testCase.settled)
			}
			if testCase.settled && err != nil {
				t.Fatalf("settled evidence returned %v", err)
			}
			if !testCase.settled && err == nil {
				t.Fatal("unsettled evidence returned no error")
			}
			if report["threshold"] != float64(testCase.threshold) {
				t.Fatalf("threshold %v, want %d", report["threshold"], testCase.threshold)
			}
		})
	}
}

// The answer is written before the verdict is returned, the shadow-compare
// convention: a caller reading only the exit status must not be able to get a
// settled one from an answer nobody could read.
func TestShadowEvidenceWritesTheAnswerBeforeReportingItIsUnsettled(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	var out bytes.Buffer
	err := githubShadowEvidence(strings.NewReader(shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(task, evidenceHeadOne, "failure", "success"))), &out)
	if err == nil {
		t.Fatal("a conflict returned no error")
	}
	report := decodeShadowEvidence(t, out.String())
	conflicting, _ := report["conflicting"].([]any)
	if len(conflicting) != 1 || conflicting[0] != task {
		t.Fatalf("conflicting %v, want only %s", report["conflicting"], task)
	}
	tasks, _ := report["tasks"].([]any)
	if len(tasks) != 1 {
		t.Fatalf("tasks %v, want one", report["tasks"])
	}
	first, _ := tasks[0].(map[string]any)
	commits, _ := first["conflicting_commits"].([]any)
	if len(commits) != 1 || commits[0] != evidenceHeadOne {
		t.Fatalf("conflicting_commits %v, want only %s", first["conflicting_commits"], evidenceHeadOne)
	}
}

// One conflict holds the task however many clean pull requests follow it. The
// decision is whether ra8ci has ever disagreed with Actions about a merge, not
// what share of the time it agreed.
func TestShadowEvidenceKeepsAConflictingTaskOutOfReady(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	var out bytes.Buffer
	err := githubShadowEvidence(strings.NewReader(shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(task, evidenceHeadOne, "failure", "success"),
		shadowEvidenceCommit(task, evidenceHeadTwo, "success", "success"),
		shadowEvidenceCommit(task, evidenceHeadThree, "success", "success"))), &out)
	if err == nil {
		t.Fatal("evidence with an unexplained conflict settled")
	}
	report := decodeShadowEvidence(t, out.String())
	ready, _ := report["ready"].([]any)
	if len(ready) != 0 {
		t.Fatalf("ready %v with a conflict outstanding", report["ready"])
	}
}

// A task a commit did not exercise is not evidence for that task, and is
// reported per commit so an under-observed task can be explained without
// going back to the inputs.
func TestShadowEvidenceNamesWhatEachCommitDidNotExercise(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	var out bytes.Buffer
	if err := githubShadowEvidence(strings.NewReader(shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(task, evidenceHeadOne, "success", "success"))), &out); err != nil {
		t.Fatalf("githubShadowEvidence: %v", err)
	}
	report := decodeShadowEvidence(t, out.String())
	commits, _ := report["commits"].([]any)
	if len(commits) != 1 {
		t.Fatalf("commits %v, want one", report["commits"])
	}
	first, _ := commits[0].(map[string]any)
	if first["head_sha"] != evidenceHeadOne {
		t.Fatalf("head_sha %v, want %s", first["head_sha"], evidenceHeadOne)
	}
	if first["clean"] != true {
		t.Fatalf("clean %v, want true", first["clean"])
	}
	if _, ok := first["not_exercised"].([]any); !ok {
		t.Fatalf("not_exercised %v is not a list", first["not_exercised"])
	}
	if strings.Contains(out.String(), "null") {
		t.Fatalf("report carries a null field:\n%s", out.String())
	}
}

// The threshold is the operator's judgement and is stated, never defaulted. A
// default would answer a question nobody asked while looking like an answer to
// the one they did.
func TestShadowEvidenceRefusesInputItCannotReadAsEvidence(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	good := shadowEvidenceCommit(task, evidenceHeadOne, "success", "success")
	for _, testCase := range []struct {
		name  string
		input string
	}{
		{"not an object", `["commits"]`},
		{"unknown field", `{"threshold":1,"commits":[` + good + `],"note":"x"}`},
		{"trailing document", shadowEvidenceInputDocument(1, good) + `{"threshold":1}`},
		{"no threshold", `{"commits":[` + good + `]}`},
		{"threshold below one", shadowEvidenceInputDocument(0, good)},
		{"no commits", `{"threshold":1,"commits":[]}`},
		{"one commit twice", shadowEvidenceInputDocument(1, good, good)},
		{"unknown conclusion", shadowEvidenceInputDocument(1,
			shadowEvidenceCommit(task, evidenceHeadOne, "success", "exploded"))},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			var out bytes.Buffer
			if err := githubShadowEvidence(strings.NewReader(testCase.input), &out); err == nil {
				t.Fatal("refusable input was accepted")
			}
			if out.Len() != 0 {
				t.Fatalf("a refused answer wrote to the stream:\n%s", out.String())
			}
		})
	}
}

func TestShadowEvidenceRefusesWhenCheckRunPublishingIsNotConfigured(t *testing.T) {
	t.Setenv(github.EnvShadowCorrespondenceFile, "")
	os.Unsetenv(github.EnvShadowCorrespondenceFile)
	t.Setenv(github.EnvCheckRunMode, "")
	os.Unsetenv(github.EnvCheckRunMode)
	var out bytes.Buffer
	err := githubShadowEvidence(strings.NewReader(`{"threshold":1,"commits":[]}`), &out)
	if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
		t.Fatalf("error %v, want one naming %s", err, github.EnvShadowCorrespondenceFile)
	}
	if out.Len() != 0 {
		t.Fatalf("a refused answer wrote to the stream:\n%s", out.String())
	}
}

// The mode decides what a deployment may do with the answer, never how the
// evidence is graded.
func TestShadowEvidenceGradesTheSameEvidenceInEitherMode(t *testing.T) {
	var pages []string
	for _, mode := range []string{"shadow", "authoritative"} {
		task := requiredChecksEnv(t, mode)
		var out bytes.Buffer
		if err := githubShadowEvidence(strings.NewReader(shadowEvidenceInputDocument(1,
			shadowEvidenceCommit(task, evidenceHeadOne, "success", "success"))), &out); err != nil {
			t.Fatalf("mode %s: %v", mode, err)
		}
		report := decodeShadowEvidence(t, out.String())
		if report["mode"] != mode {
			t.Fatalf("mode %v, want %s", report["mode"], mode)
		}
		if report["may_block_merges"] != (mode == "authoritative") {
			t.Fatalf("mode %s: may_block_merges %v", mode, report["may_block_merges"])
		}
		delete(report, "mode")
		delete(report, "may_block_merges")
		encoded, err := json.Marshal(report)
		if err != nil {
			t.Fatalf("re-encode: %v", err)
		}
		pages = append(pages, string(encoded))
	}
	if pages[0] != pages[1] {
		t.Fatalf("the same evidence graded differently by mode:\n%s\n%s", pages[0], pages[1])
	}
}

func TestShadowEvidenceIsDispatchedByName(t *testing.T) {
	if err := githubCommand(context.Background(), []string{"shadow-evidenc"}); err == nil ||
		!strings.Contains(err.Error(), "shadow-evidence") {
		t.Fatalf("error %v, want a usage line naming shadow-evidence", err)
	}
}

func actionsRunEnv(t *testing.T) string {
	t.Helper()
	return publishCheckRunEnv(t)
}

// actions-run needs both halves of the configuration, and names the variable
// that is missing rather than the one that happens to be checked first.
func TestActionsRunNeedsBothHalvesOfTheConfiguration(t *testing.T) {
	t.Run("no correspondence", func(t *testing.T) {
		var out bytes.Buffer
		err := githubActionsRun(context.Background(), strings.NewReader(`{"run_id":41}`), &out)
		if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
			t.Fatalf("err = %v", err)
		}
		if out.Len() != 0 {
			t.Fatalf("refused read wrote %q", out.String())
		}
	})
	t.Run("no repository", func(t *testing.T) {
		shadowCompareEnv(t, "build")
		var out bytes.Buffer
		err := githubActionsRun(context.Background(), strings.NewReader(`{"run_id":41}`), &out)
		if err == nil || !strings.Contains(err.Error(), github.EnvCheckRunRepository) {
			t.Fatalf("err = %v", err)
		}
		if out.Len() != 0 {
			t.Fatalf("refused read wrote %q", out.String())
		}
	})
}

// The document is read and refused before a reader is built, so a bad document
// is reported as a bad document rather than as a failure to reach GitHub. The
// environment here names a private key file that does not exist, so any case
// that got as far as building a reader would fail differently.
func TestActionsRunRefusesTheDocumentBeforeBuildingAReader(t *testing.T) {
	task := actionsRunEnv(t)
	plane := `[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}]`
	cases := map[string]string{
		"not an object":     `["run"]`,
		"unknown field":     `{"run_id":41,"plane":` + plane + `,"actions":[]}`,
		"trailing document": `{"run_id":41,"plane":` + plane + `} {"run_id":42}`,
		"no run":            `{"plane":` + plane + `}`,
		"negative run":      `{"run_id":-3,"plane":` + plane + `}`,
		"no plane":          `{"run_id":41}`,
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			var out bytes.Buffer
			err := githubActionsRun(context.Background(), strings.NewReader(input), &out)
			if err == nil {
				t.Fatal("document accepted")
			}
			if strings.Contains(err.Error(), "private key") {
				t.Fatalf("built a reader before refusing the document: %v", err)
			}
			if out.Len() != 0 {
				t.Fatalf("refused read wrote %q", out.String())
			}
		})
	}
}

// The comparison document accepts the anchor actions-run writes, and grades
// the same evidence with or without it: an anchor is provenance, never a term
// in the comparison.
func TestShadowCompareAcceptsTheRunAnchorWithoutRegrading(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	plane := `"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}]`
	actions := `"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"}]`
	anchored := `{"actions_run":{"run_id":41,"attempt":2,"head_sha":"` + shadowCompareHead + `"},` + plane + `,` + actions + `}`
	bare := `{` + plane + `,` + actions + `}`

	var withAnchor, without bytes.Buffer
	if err := githubShadowCompare(strings.NewReader(anchored), &withAnchor); err != nil {
		t.Fatalf("anchored comparison refused: %v", err)
	}
	if err := githubShadowCompare(strings.NewReader(bare), &without); err != nil {
		t.Fatalf("bare comparison refused: %v", err)
	}
	if withAnchor.String() != without.String() {
		t.Fatalf("the anchor changed the page:\n%s\n---\n%s", withAnchor.String(), without.String())
	}
}

// An anchor naming another commit is refused rather than ignored. It is how one
// run's outcomes end up filed under another run's attempt, and an attempt that
// does not describe the evidence under it is worse than no attempt at all.
func TestShadowCompareRefusesAnAnchorFromAnotherCommit(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	other := "89abcdef0123456789abcdef0123456789abcdef"
	input := `{"actions_run":{"run_id":41,"attempt":1,"head_sha":"` + other + `"},` +
		`"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"}]}`

	var out bytes.Buffer
	err := githubShadowCompare(strings.NewReader(input), &out)
	if err == nil || !strings.Contains(err.Error(), other) {
		t.Fatalf("err = %v, want a refusal naming %s", err, other)
	}
	if out.Len() != 0 {
		t.Fatalf("refused comparison wrote a page: %q", out.String())
	}
}

// The subcommand is dispatched by name, and a misspelling is refused with a
// usage line that names the real one.
func TestActionsRunIsDispatchedByName(t *testing.T) {
	err := githubCommand(context.Background(), []string{"actions-runs"})
	if err == nil || !strings.Contains(err.Error(), "actions-run") {
		t.Fatalf("err = %v", err)
	}
}

// evidenceGateEnv declares a correspondence for two catalog tasks in
// authoritative mode, the only mode in which the evidence decides anything.
func evidenceGateEnv(t *testing.T) (string, string) {
	t.Helper()
	loaded, err := catalog.Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	names := loaded.Names()
	if len(names) < 2 {
		t.Skip("catalog carries fewer than two tasks")
	}
	t.Setenv(github.EnvCheckRunMode, "authoritative")
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t,
		`{"`+names[0]+`":"build","`+names[1]+`":"lint"}`))
	return names[0], names[1]
}

// evidenceCommit is one pull request's comparisons: each task observed with the
// given conclusion and its Actions job concluding the same way.
func evidenceCommit(head, task, job, conclusion string) string {
	return `{"plane":[{"task":"` + task + `","head_sha":"` + head + `","observed":"` + conclusion + `"}],` +
		`"actions":[{"job":"` + job + `","head_sha":"` + head + `","conclusion":"` + conclusion + `"}]}`
}

func decodeEvidenceGate(t *testing.T, out *bytes.Buffer) map[string]any {
	t.Helper()
	var report map[string]any
	if err := json.Unmarshal(out.Bytes(), &report); err != nil {
		t.Fatalf("decode evidence gate plan: %v\nplan: %s", err, out.String())
	}
	return report
}

const (
	evidenceGateHeadA = "1111111111111111111111111111111111111111"
	evidenceGateHeadB = "2222222222222222222222222222222222222222"
)

// The gate is planned for the tasks the evidence backs, and a task compared
// fewer times than the threshold asked for is held back and named.
func TestEvidenceGateProposesOnlyWhatTheEvidenceBacks(t *testing.T) {
	backed, thin := evidenceGateEnv(t)
	input := `{"threshold":2,"required":[],"commits":[` +
		evidenceCommit(evidenceGateHeadA, backed, "build", "success") + `,` +
		`{"plane":[{"task":"` + backed + `","head_sha":"` + evidenceGateHeadB + `","observed":"success"},` +
		`{"task":"` + thin + `","head_sha":"` + evidenceGateHeadB + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + evidenceGateHeadB + `","conclusion":"success"},` +
		`{"job":"lint","head_sha":"` + evidenceGateHeadB + `","conclusion":"success"}]}` +
		`]}`

	var out bytes.Buffer
	err := githubEvidenceGate(strings.NewReader(input), &out)
	if err == nil || !strings.Contains(err.Error(), "does not back") {
		t.Fatalf("a held-back task reported as a finished plan: %v", err)
	}

	report := decodeEvidenceGate(t, &out)
	add, _ := report["add"].([]any)
	if len(add) != 1 || add[0] != "ra8ci / "+backed {
		t.Fatalf("add = %v, want only the backed task", report["add"])
	}
	withheld, _ := report["withheld"].([]any)
	if len(withheld) != 1 {
		t.Fatalf("withheld = %v, want the thin task named", report["withheld"])
	}
	held, _ := withheld[0].(map[string]any)
	if held["task"] != thin || held["reason"] != "insufficient" || held["already_required"] != false {
		t.Fatalf("withheld entry = %v, want %q held as insufficient", held, thin)
	}
}

// A plan that holds nothing back is a clean exit, so a pipeline can tell a
// finished plan from a partial one by its status alone.
func TestEvidenceGateExitsCleanWhenNothingIsHeldBack(t *testing.T) {
	backed, _ := evidenceGateEnv(t)
	input := `{"threshold":2,"required":[],"commits":[` +
		evidenceCommit(evidenceGateHeadA, backed, "build", "success") + `,` +
		evidenceCommit(evidenceGateHeadB, backed, "build", "success") + `]}`

	var out bytes.Buffer
	if err := githubEvidenceGate(strings.NewReader(input), &out); err != nil {
		t.Fatalf("fully backed plan refused: %v", err)
	}
	report := decodeEvidenceGate(t, &out)
	if report["settled"] != true {
		t.Fatalf("settled = %v, want true", report["settled"])
	}
	if withheld, _ := report["withheld"].([]any); len(withheld) != 0 {
		t.Fatalf("withheld = %v, want none", report["withheld"])
	}
	if report["no_change"] != false {
		t.Fatalf("no_change = %v, want the addition to count as a change", report["no_change"])
	}
}

// The plan is written before the verdict is returned: a caller reading only the
// exit status must not be able to get a clean one from a plan nobody could read.
func TestEvidenceGateWritesThePlanBeforeReportingWhatItHeldBack(t *testing.T) {
	backed, _ := evidenceGateEnv(t)
	input := `{"threshold":5,"required":[],"commits":[` +
		evidenceCommit(evidenceGateHeadA, backed, "build", "success") + `]}`

	var out bytes.Buffer
	if err := githubEvidenceGate(strings.NewReader(input), &out); err == nil {
		t.Fatal("an unbacked plan reported as finished")
	}
	if !strings.Contains(out.String(), backed) {
		t.Fatalf("plan wrote nothing about %q: %q", backed, out.String())
	}
}

// A conflict is not more pull requests to wait for, and the plan says which of
// the two kinds of work the task is waiting on.
func TestEvidenceGateNamesAConflictAsAConflict(t *testing.T) {
	backed, _ := evidenceGateEnv(t)
	input := `{"threshold":1,"required":[],"commits":[` +
		`{"plane":[{"task":"` + backed + `","head_sha":"` + evidenceGateHeadA + `","observed":"failure"}],` +
		`"actions":[{"job":"build","head_sha":"` + evidenceGateHeadA + `","conclusion":"success"}]}` + `]}`

	var out bytes.Buffer
	if err := githubEvidenceGate(strings.NewReader(input), &out); err == nil {
		t.Fatal("a conflicting task planned onto the gate")
	}
	report := decodeEvidenceGate(t, &out)
	withheld, _ := report["withheld"].([]any)
	if len(withheld) != 1 {
		t.Fatalf("withheld = %v, want the conflicting task", report["withheld"])
	}
	if held, _ := withheld[0].(map[string]any); held["reason"] != "conflicting" {
		t.Fatalf("reason = %v, want conflicting", held["reason"])
	}
	if add, _ := report["add"].([]any); len(add) != 0 {
		t.Fatalf("add = %v, want nothing added", report["add"])
	}
}

// Withholding decides whether this plane asks for a NEW gate. A context the
// operator already requires is kept and reported, never proposed for removal.
func TestEvidenceGateKeepsAGateTheOperatorAlreadyHas(t *testing.T) {
	backed, _ := evidenceGateEnv(t)
	context := "ra8ci / " + backed
	input := `{"threshold":4,"required":["` + context + `"],"commits":[` +
		evidenceCommit(evidenceGateHeadA, backed, "build", "success") + `]}`

	var out bytes.Buffer
	if err := githubEvidenceGate(strings.NewReader(input), &out); err == nil {
		t.Fatal("an unbacked plan reported as finished")
	}
	report := decodeEvidenceGate(t, &out)
	keep, _ := report["keep"].([]any)
	if len(keep) != 1 || keep[0] != context {
		t.Fatalf("keep = %v, want %q", report["keep"], context)
	}
	if remove, _ := report["remove"].([]any); len(remove) != 0 {
		t.Fatalf("remove = %v, want the evidence never to take a gate off", report["remove"])
	}
	withheld, _ := report["withheld"].([]any)
	held, _ := withheld[0].(map[string]any)
	if held["already_required"] != true {
		t.Fatalf("already_required = %v, want the report to say the gate runs ahead of the evidence", held)
	}
}

// A shadow deployment proposes nothing whatever the evidence says, and holds
// nothing back either: the mode refused, not the evidence.
func TestEvidenceGateInShadowModeProposesAndWithholdsNothing(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	input := `{"threshold":1,"required":[],"commits":[` +
		evidenceCommit(evidenceGateHeadA, task, "build", "success") + `]}`

	var out bytes.Buffer
	if err := githubEvidenceGate(strings.NewReader(input), &out); err != nil {
		t.Fatalf("shadow plan refused: %v", err)
	}
	report := decodeEvidenceGate(t, &out)
	if report["mode"] != "shadow" || report["may_block_merges"] != false {
		t.Fatalf("mode = %v / %v, want a shadow deployment", report["mode"], report["may_block_merges"])
	}
	if add, _ := report["add"].([]any); len(add) != 0 {
		t.Fatalf("add = %v, want none in shadow mode", report["add"])
	}
	if withheld, _ := report["withheld"].([]any); len(withheld) != 0 {
		t.Fatalf("withheld = %v, want none: the mode refused, not the evidence", report["withheld"])
	}
}

func TestEvidenceGateRefusesInputItCannotReadAsOneAsk(t *testing.T) {
	backed, _ := evidenceGateEnv(t)
	commit := evidenceCommit(evidenceGateHeadA, backed, "build", "success")
	cases := map[string]string{
		"not an object":       `[` + commit + `]`,
		"unknown field":       `{"threshold":1,"commits":[` + commit + `],"gate":[]}`,
		"trailing document":   `{"threshold":1,"commits":[` + commit + `]}{"threshold":1}`,
		"no threshold":        `{"commits":[` + commit + `]}`,
		"threshold below one": `{"threshold":0,"commits":[` + commit + `]}`,
		"no commits":          `{"threshold":1,"commits":[]}`,
		"repeated commit":     `{"threshold":1,"commits":[` + commit + `,` + commit + `]}`,
		"padded context":      `{"threshold":1,"required":[" CodeQL"],"commits":[` + commit + `]}`,
		"same context twice":  `{"threshold":1,"required":["CodeQL","CodeQL"],"commits":[` + commit + `]}`,
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			var out bytes.Buffer
			if err := githubEvidenceGate(strings.NewReader(input), &out); err == nil {
				t.Fatalf("unreadable input accepted, wrote %q", out.String())
			}
			if out.Len() != 0 {
				t.Fatalf("refused ask wrote %q", out.String())
			}
		})
	}
}

func TestEvidenceGateRefusesWhenCheckRunPublishingIsNotConfigured(t *testing.T) {
	t.Setenv(github.EnvShadowCorrespondenceFile, "")
	os.Unsetenv(github.EnvShadowCorrespondenceFile)
	t.Setenv(github.EnvCheckRunMode, "")
	os.Unsetenv(github.EnvCheckRunMode)

	var out bytes.Buffer
	err := githubEvidenceGate(strings.NewReader(`{"threshold":1,"commits":[]}`), &out)
	if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
		t.Fatalf("error = %v, want one naming %s", err, github.EnvShadowCorrespondenceFile)
	}
	if out.Len() != 0 {
		t.Fatalf("refusal wrote %q", out.String())
	}
}

func TestEvidenceGateIsDispatchedByName(t *testing.T) {
	ctx := context.Background()
	if err := githubCommand(ctx, []string{"evidence gate"}); err == nil ||
		!strings.Contains(err.Error(), "evidence-gate") {
		t.Fatalf("error = %v, want a usage line naming evidence-gate", err)
	}
}

// plannedRun builds one planned check run through NewTaskCheckRun, so the
// reconciliation is exercised against runs the publisher would actually post
// rather than hand-set values.
func plannedRun(t *testing.T, mode github.CheckRunMode, task, state string) plannedCheckRun {
	t.Helper()
	run, err := github.NewTaskCheckRun(mode, task, shadowCompareHead, state)
	if err != nil {
		t.Fatalf("build the intended run: %v", err)
	}
	return plannedCheckRun{Task: task, Run: run, Summary: "ra8ci observed " + state}
}

// publishedAs renders one planned run as a run this plane already published,
// carrying the external identifier the publisher posts. Without it the run is
// one somebody else left under our name, which the reconciliation reads as a
// collision; that case has its own test below.
func publishedAs(plan plannedCheckRun, id int64, status, conclusion, title string) github.PublishedCheckRun {
	identifier, _ := github.CheckRunExternalID(plan.Run)
	return github.PublishedCheckRun{
		ID: id, Name: plan.Run.Name, Mode: plan.Run.Mode,
		Status: status, Conclusion: conclusion, Title: title, ExternalID: identifier,
	}
}

// listing builds the commit listing the reconciliation reads.
func listing(runs ...github.PublishedCheckRun) github.PublishedCheckRuns {
	return github.PublishedCheckRuns{HeadSHA: shadowCompareHead, Runs: runs}
}

func TestReconciledPlanPostsOnlyWhatIsNotAlreadyPublished(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	cases := map[string]struct {
		published github.PublishedCheckRuns
		want      github.PublishDecision
		repeat    bool
	}{
		"nothing on the commit": {listing(), github.PublishNeeded, true},
		"the same run already there": {
			listing(publishedAs(plan, 41, "completed", plan.Run.Conclusion, plan.Run.Title)),
			github.PublishSettled, false,
		},
		"a write still in flight": {
			listing(publishedAs(plan, 42, "in_progress", "", "")),
			github.PublishInFlight, false,
		},
	}
	for name, test := range cases {
		t.Run(name, func(t *testing.T) {
			reconciled, err := reconcileCheckRunPlan([]plannedCheckRun{plan}, test.published)
			if err != nil {
				t.Fatalf("reconcile: %v", err)
			}
			if len(reconciled) != 1 {
				t.Fatalf("reconciled %d runs, want 1", len(reconciled))
			}
			if reconciled[0].Verdict.Decision != test.want {
				t.Fatalf("decision %s, want %s", reconciled[0].Verdict.Decision, test.want)
			}
			if reconciled[0].Verdict.Repeat() != test.repeat {
				t.Fatalf("repeat %v, want %v", reconciled[0].Verdict.Repeat(), test.repeat)
			}
		})
	}
}

func TestOneDisagreeingRunRefusesTheWholeDocument(t *testing.T) {
	task := firstCatalogTask(t)
	disagreeing := plannedRun(t, github.ModeAuthoritative, task, "failed")
	agreeing := plannedRun(t, github.ModeAuthoritative, task, "succeeded")
	published := listing(publishedAs(agreeing, 77, "completed", agreeing.Run.Conclusion, agreeing.Run.Title))
	reconciled, err := reconcileCheckRunPlan([]plannedCheckRun{disagreeing}, published)
	if err == nil {
		t.Fatalf("reconciled %v, want a refusal", reconciled)
	}
	if reconciled != nil {
		t.Fatalf("a refused reconciliation carried %d runs, want none", len(reconciled))
	}
	for _, want := range []string{task, "#77", shadowCompareHead} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("error %q names neither the task, the run nor the commit: want %q", err, want)
		}
	}
}

func TestAShadowRunIsSettledOnlyByItsOwnObservation(t *testing.T) {
	task := firstCatalogTask(t)
	failed := plannedRun(t, github.ModeShadow, task, "failed")
	succeeded := plannedRun(t, github.ModeShadow, task, "succeeded")
	if failed.Run.Conclusion != succeeded.Run.Conclusion {
		t.Fatalf("shadow conclusions differ (%q, %q); this test is about the title carrying the observation",
			failed.Run.Conclusion, succeeded.Run.Conclusion)
	}
	published := listing(publishedAs(succeeded, 9, "completed", succeeded.Run.Conclusion, succeeded.Run.Title))
	if _, err := reconcileCheckRunPlan([]plannedCheckRun{failed}, published); err == nil {
		t.Fatal("a shadow run reporting the opposite observation was settled by it")
	}
	same := listing(publishedAs(failed, 9, "completed", failed.Run.Conclusion, failed.Run.Title))
	reconciled, err := reconcileCheckRunPlan([]plannedCheckRun{failed}, same)
	if err != nil {
		t.Fatalf("reconcile the same observation: %v", err)
	}
	if reconciled[0].Verdict.Decision != github.PublishSettled {
		t.Fatalf("decision %s, want settled", reconciled[0].Verdict.Decision)
	}
}

func TestReconcileNamesTheTaskWhenTheListingIsAboutAnotherCommit(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "succeeded")
	elsewhere := github.PublishedCheckRuns{HeadSHA: "89abcdef0123456789abcdef0123456789abcdef"}
	_, err := reconcileCheckRunPlan([]plannedCheckRun{plan}, elsewhere)
	if err == nil || !strings.Contains(err.Error(), task) {
		t.Fatalf("error %v, want one naming %s", err, task)
	}
	if !errors.Is(err, github.ErrReconcileCommitMismatch) {
		t.Fatalf("error %v, want a commit mismatch", err)
	}
}

func TestAnEmptyPlanIsRefusedRatherThanPublishedAsNothing(t *testing.T) {
	if _, err := reconcileCheckRunPlan(nil, listing()); err == nil {
		t.Fatal("an empty plan reconciled without complaint")
	}
}

func TestEveryDecisionIsOneWordInTheReport(t *testing.T) {
	for _, decision := range []github.PublishDecision{
		github.PublishNeeded, github.PublishSettled, github.PublishInFlight, github.PublishConflicts,
	} {
		token := decisionToken(decision)
		if token == "" || strings.ContainsAny(token, " \t") {
			t.Fatalf("decision %s renders as %q, which a line-oriented report cannot carry", decision, token)
		}
	}
}

func TestAnUnfinishedRunIsReportedByItsStatus(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeShadow, task, "succeeded")
	queued := publishedAs(plan, 5, "queued", "", "")
	if got := publishedState(queued); got != "queued" {
		t.Fatalf("state %q, want the status of a run with no conclusion", got)
	}
	finished := publishedAs(plan, 5, "completed", plan.Run.Conclusion, plan.Run.Title)
	if got := publishedState(finished); got != plan.Run.Conclusion {
		t.Fatalf("state %q, want the conclusion %q", got, plan.Run.Conclusion)
	}
	if !strings.Contains(describePublishedRuns([]github.PublishedCheckRun{queued}), "#5 queued") {
		t.Fatalf("description %q does not point at the run", describePublishedRuns([]github.PublishedCheckRun{queued}))
	}
}

// A check run name is public, so a run under one of ours may have been posted
// by something else entirely. The refusal has to say so: telling an operator
// the commit disagrees would send them through this deployment's own history
// looking for a run that was never in it.
func TestARunThisPlaneDidNotPublishRefusesTheDocument(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	theirs := publishedAs(plan, 61, "completed", plan.Run.Conclusion, plan.Run.Title)
	theirs.ExternalID = ""
	_, err := reconcileCheckRunPlan([]plannedCheckRun{plan}, listing(theirs))
	if err == nil {
		t.Fatal("a run under our name with no identifier of ours was accepted")
	}
	if !strings.Contains(err.Error(), "did not publish") {
		t.Fatalf("refusal %q does not say who posted the run", err)
	}
	if !strings.Contains(err.Error(), "#61") {
		t.Fatalf("refusal %q does not point at the run", err)
	}
}

// Our own runs disagreeing about one commit is the other conflict, and it
// reads differently: the runs are ours, and what they say is the question.
func TestOurOwnDisagreementStillReadsAsADisagreement(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	_, err := reconcileCheckRunPlan([]plannedCheckRun{plan}, listing(publishedAs(plan, 62, "completed", "success", "")))
	if err == nil {
		t.Fatal("a disagreeing run of ours was accepted")
	}
	if !strings.Contains(err.Error(), "saying something else") {
		t.Fatalf("refusal %q does not read as a disagreement", err)
	}
	if strings.Contains(err.Error(), "did not publish") {
		t.Fatalf("refusal %q blames a stranger for our own run", err)
	}
}

// The survey is the read publish-check-run tells an operator to make when it
// leaves a task to a write already in flight. Every task is reported,
// including the one that conflicts: nothing is posted here, so withholding
// the rest would only hide the picture the read was made for.
func TestTheSurveyReportsEveryTaskIncludingTheConflictingOne(t *testing.T) {
	task := firstCatalogTask(t)
	settled := plannedRun(t, github.ModeAuthoritative, task, "failed")
	stranger := publishedAs(settled, 71, "completed", settled.Run.Conclusion, settled.Run.Title)
	stranger.ExternalID = ""

	report, err := surveyCheckRunPlan([]plannedCheckRun{settled}, listing(stranger))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	if report.Conflict != 1 || report.Settled {
		t.Fatalf("report = %+v", report)
	}
	if len(report.Tasks) != 1 || report.Tasks[0].Decision != "conflicts" {
		t.Fatalf("tasks = %+v", report.Tasks)
	}
	if len(report.Tasks[0].Published) != 1 || report.Tasks[0].Published[0].Ours {
		t.Fatalf("published = %+v", report.Tasks[0].Published)
	}
	if report.Commit != settled.Run.HeadSHA || report.Mode != settled.Run.Mode.String() {
		t.Fatalf("report = %+v", report)
	}
}

// A run this plane published and a run it did not read identically in a
// listing. Saying which is which is the whole of why one of them conflicts.
func TestTheSurveySaysWhichRunsAreOurs(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	ours := publishedAs(plan, 72, "completed", plan.Run.Conclusion, plan.Run.Title)
	if ours.ExternalID == "" {
		t.Fatal("a run we published carries no identifier")
	}
	theirs := ours
	theirs.ID = 73
	theirs.ExternalID = "ra8ci-1-" + strings.Repeat("0", 32)

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(ours, theirs))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	claimed := map[int64]bool{}
	for _, run := range report.Tasks[0].Published {
		claimed[run.ID] = run.Ours
		if run.ExternalID == "" {
			t.Fatalf("run #%d reported without its identifier", run.ID)
		}
	}
	if !claimed[72] || claimed[73] {
		t.Fatalf("claimed = %+v", claimed)
	}
}

// Settled is the whole commit's answer, and it is the strict one: a run still
// to post and a run still in flight are both work not yet done.
func TestTheSurveyIsSettledOnlyWhenNothingIsLeft(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	cases := map[string]struct {
		published github.PublishedCheckRuns
		settled   bool
		decision  string
	}{
		"nothing published yet": {listing(), false, "needed"},
		"a write in flight":     {listing(publishedAs(plan, 74, "queued", "", "")), false, "in-flight"},
		"already published": {
			listing(publishedAs(plan, 75, "completed", plan.Run.Conclusion, plan.Run.Title)),
			true, "settled",
		},
	}
	for name, test := range cases {
		t.Run(name, func(t *testing.T) {
			report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, test.published)
			if err != nil {
				t.Fatalf("survey: %v", err)
			}
			if report.Settled != test.settled {
				t.Fatalf("settled %v, want %v", report.Settled, test.settled)
			}
			if report.Tasks[0].Decision != test.decision {
				t.Fatalf("decision %q, want %q", report.Tasks[0].Decision, test.decision)
			}
		})
	}
}

// A run under a name the document does not plan is accounted for by nothing
// above, so the survey reports it apart from the tasks. This is the leftover
// a retired or renamed task leaves on every commit it was published to.
func TestTheSurveyReportsARunNoTaskPlans(t *testing.T) {
	first, second := twoCatalogTasks(t)
	plan := plannedRun(t, github.ModeAuthoritative, first, "failed")
	retired := plannedRun(t, github.ModeAuthoritative, second, "succeeded")
	ours := publishedAs(plan, 80, "completed", plan.Run.Conclusion, plan.Run.Title)
	leftover := publishedAs(retired, 81, "completed", retired.Run.Conclusion, retired.Run.Title)

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(ours, leftover))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	if report.Unplanned != 1 || len(report.UnplannedRun) != 1 {
		t.Fatalf("report = %+v", report)
	}
	reported := report.UnplannedRun[0]
	if reported.ID != 81 || reported.Name != retired.Run.Name || reported.Mode != retired.Run.Mode.String() {
		t.Fatalf("unplanned = %+v", reported)
	}
	if reported.Status != "completed" || reported.Conclusion != retired.Run.Conclusion || reported.Title != retired.Run.Title {
		t.Fatalf("unplanned = %+v", reported)
	}
	if len(report.Tasks) != 1 || len(report.Tasks[0].Published) != 1 || report.Tasks[0].Published[0].ID != 80 {
		t.Fatalf("tasks = %+v", report.Tasks)
	}
}

// A leftover run of ours and somebody else's run under one of our names read
// alike in a listing and send an operator to different places, so the report
// says which it is without an intended run to compare against.
func TestAnUnplannedRunSaysWhetherItIsOurs(t *testing.T) {
	first, second := twoCatalogTasks(t)
	plan := plannedRun(t, github.ModeAuthoritative, first, "failed")
	retired := plannedRun(t, github.ModeAuthoritative, second, "succeeded")
	mine := publishedAs(retired, 82, "completed", retired.Run.Conclusion, retired.Run.Title)
	stranger := mine
	stranger.ID = 83
	stranger.ExternalID = "ra8ci-1-" + strings.Repeat("0", 32)
	blank := mine
	blank.ID = 84
	blank.ExternalID = ""

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(mine, stranger, blank))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	claimed := map[int64]bool{}
	for _, run := range report.UnplannedRun {
		claimed[run.ID] = run.Ours
	}
	if len(claimed) != 3 || !claimed[82] || claimed[83] || claimed[84] {
		t.Fatalf("claimed = %+v", claimed)
	}
}

// Ours is one bit and there are four ways a run is not ours, so the survey
// reports the longer answer beside it. A stranger's run, our own work from a
// deployment that derives identifiers differently, and a run of ours derived
// for another commit all report ours false and are different work.
func TestTheSurveySaysWhichWayARunIsNotOurs(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	ours := publishedAs(plan, 90, "completed", plan.Run.Conclusion, plan.Run.Title)
	elsewhere, err := github.CheckRunExternalID(github.TaskCheckRun{
		Name: plan.Run.Name, HeadSHA: "77777777aaaaaaaa55555555cccccccc99999999",
	})
	if err != nil {
		t.Fatalf("identifier for another commit: %v", err)
	}
	runs := []github.PublishedCheckRun{ours}
	for id, identifier := range map[int64]string{
		91: "",
		92: "jenkins-build-4417",
		93: "ra8ci-2-" + strings.Repeat("a", 32),
		94: elsewhere,
	} {
		run := ours
		run.ID = id
		run.ExternalID = identifier
		runs = append(runs, run)
	}

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(runs...))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	want := map[int64]string{
		90: "ours", 91: "absent", 92: "foreign", 93: "superseded", 94: "other subject",
	}
	for _, run := range report.Tasks[0].Published {
		if run.Identifier != want[run.ID] {
			t.Fatalf("run #%d identifier = %q, want %q", run.ID, run.Identifier, want[run.ID])
		}
		if run.Ours != (run.Identifier == "ours") {
			t.Fatalf("run #%d: ours = %v beside identifier %q", run.ID, run.Ours, run.Identifier)
		}
	}
	if len(report.Tasks[0].Published) != len(want) {
		t.Fatalf("published = %d runs, want %d", len(report.Tasks[0].Published), len(want))
	}
}

// The same longer answer on the unplanned list, where it matters most: a
// leftover of ours under a retired name is a name to clean up, and a run of
// ours derived for another commit was posted against the wrong commit.
func TestAnUnplannedRunSaysWhichWayItIsNotOurs(t *testing.T) {
	first, second := twoCatalogTasks(t)
	plan := plannedRun(t, github.ModeAuthoritative, first, "failed")
	retired := plannedRun(t, github.ModeAuthoritative, second, "succeeded")
	mine := publishedAs(retired, 95, "completed", retired.Run.Conclusion, retired.Run.Title)
	elsewhere, err := github.CheckRunExternalID(github.TaskCheckRun{
		Name: retired.Run.Name, HeadSHA: "77777777aaaaaaaa55555555cccccccc99999999",
	})
	if err != nil {
		t.Fatalf("identifier for another commit: %v", err)
	}
	misSubjected := mine
	misSubjected.ID = 96
	misSubjected.ExternalID = elsewhere
	stranger := mine
	stranger.ID = 97
	stranger.ExternalID = "buildkite-2231"

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan},
		listing(publishedAs(plan, 98, "completed", plan.Run.Conclusion, plan.Run.Title),
			mine, misSubjected, stranger))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	want := map[int64]string{95: "ours", 96: "other subject", 97: "foreign"}
	if len(report.UnplannedRun) != len(want) {
		t.Fatalf("unplanned = %+v", report.UnplannedRun)
	}
	for _, run := range report.UnplannedRun {
		if run.Identifier != want[run.ID] {
			t.Fatalf("run #%d identifier = %q, want %q", run.ID, run.Identifier, want[run.ID])
		}
		if run.Ours != (run.Identifier == "ours") {
			t.Fatalf("run #%d: ours = %v beside identifier %q", run.ID, run.Ours, run.Identifier)
		}
	}
}

// Every reported run carries a standing: a run with no identifier says
// absent in words, so nothing in the document reads as a field that failed
// to be filled in.
func TestEveryReportedRunCarriesAStanding(t *testing.T) {
	first, second := twoCatalogTasks(t)
	plan := plannedRun(t, github.ModeAuthoritative, first, "failed")
	retired := plannedRun(t, github.ModeAuthoritative, second, "succeeded")
	blankTask := publishedAs(plan, 99, "completed", plan.Run.Conclusion, plan.Run.Title)
	blankTask.ExternalID = ""
	blankLeftover := publishedAs(retired, 100, "completed", retired.Run.Conclusion, retired.Run.Title)
	blankLeftover.ExternalID = ""

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(blankTask, blankLeftover))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	for _, run := range report.Tasks[0].Published {
		if run.Identifier != "absent" {
			t.Fatalf("task run #%d identifier = %q", run.ID, run.Identifier)
		}
	}
	for _, run := range report.UnplannedRun {
		if run.Identifier != "absent" {
			t.Fatalf("unplanned run #%d identifier = %q", run.ID, run.Identifier)
		}
	}
	encoded, err := json.Marshal(report)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if !strings.Contains(string(encoded), `"identifier":"absent"`) {
		t.Fatalf("report does not carry the standing on the wire: %s", encoded)
	}
}

// An unplanned run moves neither the settled answer nor the counts the exit
// status reads. The document is about this publish; a leftover run is a fact
// about the commit, and failing the command over one would stop a publish
// nothing is wrong with.
func TestAnUnplannedRunDoesNotUnsettleThePublish(t *testing.T) {
	first, second := twoCatalogTasks(t)
	plan := plannedRun(t, github.ModeAuthoritative, first, "failed")
	retired := plannedRun(t, github.ModeAuthoritative, second, "succeeded")
	ours := publishedAs(plan, 85, "completed", plan.Run.Conclusion, plan.Run.Title)
	leftover := publishedAs(retired, 86, "completed", retired.Run.Conclusion, retired.Run.Title)

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(ours, leftover))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	if !report.Settled || report.Conflict != 0 || report.Posting != 0 || report.Waiting != 0 {
		t.Fatalf("report = %+v", report)
	}
	if report.Unplanned != 1 {
		t.Fatalf("unplanned = %d, want the leftover still reported", report.Unplanned)
	}
}

// A commit the document accounts for reports an empty list, not a null one:
// the field is read by whatever consumes the survey, and null would have to
// be handled as a third state that never means anything different.
func TestAnAccountedForCommitReportsAnEmptyUnplannedList(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	ours := publishedAs(plan, 87, "completed", plan.Run.Conclusion, plan.Run.Title)

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(ours))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	encoded, err := json.Marshal(report)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if !strings.Contains(string(encoded), `"unplanned_runs":[]`) {
		t.Fatalf("encoded = %s", encoded)
	}
}

// An unplanned run's summary is excerpted exactly as a task's run is: the
// report is a document a person reads, and one leftover run must not be able
// to fill it.
func TestAnUnplannedRunsSummaryIsExcerpted(t *testing.T) {
	first, second := twoCatalogTasks(t)
	plan := plannedRun(t, github.ModeAuthoritative, first, "failed")
	retired := plannedRun(t, github.ModeAuthoritative, second, "succeeded")
	ours := publishedAs(plan, 88, "completed", plan.Run.Conclusion, plan.Run.Title)
	leftover := publishedAs(retired, 89, "completed", retired.Run.Conclusion, retired.Run.Title)
	leftover.Summary = strings.Repeat("e", maxReportedCheckRunSummary+7)

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(ours, leftover))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	if len(report.UnplannedRun) != 1 {
		t.Fatalf("report = %+v", report)
	}
	reported := report.UnplannedRun[0]
	if len([]rune(reported.Summary)) != maxReportedCheckRunSummary || !reported.SummaryTruncated {
		t.Fatalf("summary = %d runes, truncated %v", len([]rune(reported.Summary)), reported.SummaryTruncated)
	}
}

// A survey of no runs is a question with no answer, and it is what a document
// nobody filled in looks like.
func TestASurveyOfNoRunsIsRefused(t *testing.T) {
	if _, err := surveyCheckRunPlan(nil, listing()); err == nil {
		t.Fatal("an empty plan was surveyed")
	}
}

// The command reads and reports; a refused document leaves nothing behind, so
// a half-written survey can never be read as the commit's state.
func TestAReconcileOfARefusedDocumentWritesNothing(t *testing.T) {
	task := publishCheckRunEnv(t)
	refusals := map[string]string{
		"not an object":      `["` + task + `"]`,
		"unknown field":      `{"head_sha":"` + shadowCompareHead + `","commits":[]}`,
		"trailing document":  `{"head_sha":"` + shadowCompareHead + `","runs":[]} {}`,
		"unknown task":       `{"head_sha":"` + shadowCompareHead + `","runs":[{"task":"nowhere","state":"failed"}]}`,
		"a commit we cannot": `{"head_sha":"nope","runs":[{"task":"` + task + `","state":"failed"}]}`,
	}
	for name, document := range refusals {
		t.Run(name, func(t *testing.T) {
			var out strings.Builder
			if err := githubReconcileCheckRuns(context.Background(), strings.NewReader(document), &out); err == nil {
				t.Fatal("the document was accepted")
			}
			if out.Len() != 0 {
				t.Fatalf("wrote %q", out.String())
			}
		})
	}
}

// The survey needs the same configuration publishing does, and says which half
// is missing.
func TestReconcileNeedsTheCheckRunConfiguration(t *testing.T) {
	for _, name := range []string{github.EnvShadowCorrespondenceFile, github.EnvCheckRunMode, github.EnvCheckRunRepository} {
		t.Setenv(name, "")
		os.Unsetenv(name)
	}
	err := githubReconcileCheckRuns(context.Background(), strings.NewReader("{}"), io.Discard)
	if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
		t.Fatalf("error %v, want one naming %s", err, github.EnvShadowCorrespondenceFile)
	}
}

// The subcommand is reachable by name, and the usage line names it.
func TestReconcileIsOneOfTheGithubSubcommands(t *testing.T) {
	err := githubCommand(context.Background(), []string{"reconcil"})
	if err == nil {
		t.Fatal("a misspelled subcommand was accepted")
	}
	if !strings.Contains(err.Error(), "reconcile") {
		t.Fatalf("usage %q does not name the subcommand", err)
	}
}

// pullRequestHeadFor builds a head the report can be assembled from without
// contacting GitHub, so the wire shape is pinned on its own.
func pullRequestHeadFor(number int, head, base, state string, merged, fork bool) github.PullRequestHead {
	return github.PullRequestHead{
		Number: number, HeadSHA: head, BaseRef: base, State: state,
		Merged: merged, FromFork: fork, HeadRepository: "bsikar/ra8-firmware",
	}
}

// The report names where the pull request is and every run on its head, each
// run marked with whether `actions-run` will grade it.
func TestThePullRequestReportNamesEveryRunOnTheHead(t *testing.T) {
	head := pullRequestHeadFor(1589, shadowCompareHead, "ra8ci/dev", "open", false, false)
	report := pullRequestRunsFrom(head, github.CommitWorkflowRuns{
		HeadSHA: shadowCompareHead,
		Runs: []github.CommitWorkflowRun{
			{ID: 43, Workflow: "nightly", Attempt: 1, Event: "schedule", Status: "in_progress"},
			{ID: 41, Workflow: "checks", Attempt: 2, Event: "pull_request", Status: "completed", Conclusion: "failure"},
		},
	})
	if report.Number != 1589 || report.HeadSHA != shadowCompareHead || report.BaseRef != "ra8ci/dev" ||
		report.State != "open" || report.Merged || report.FromFork || report.HeadRepository != "bsikar/ra8-firmware" {
		t.Fatalf("report anchor = %#v", report)
	}
	if len(report.Runs) != 2 {
		t.Fatalf("report dropped a run: %#v", report.Runs)
	}
	if report.Runs[0].RunID != 43 || report.Runs[0].Gradable || report.Runs[0].Conclusion != "" {
		t.Fatalf("unfinished run = %#v", report.Runs[0])
	}
	if report.Runs[1].RunID != 41 || !report.Runs[1].Gradable || report.Runs[1].Attempt != 2 ||
		report.Runs[1].Workflow != "checks" || report.Runs[1].Conclusion != "failure" {
		t.Fatalf("completed run = %#v", report.Runs[1])
	}
	if report.Gradable != 1 {
		t.Fatalf("gradable = %d, want 1", report.Gradable)
	}
}

// A head with no runs at all is an answer, not an absence: the report says so
// with an empty list rather than a null one, so two reports diff.
func TestAPullRequestWithNoRunsIsStillAReport(t *testing.T) {
	head := pullRequestHeadFor(1590, shadowCompareHead, "ra8ci/dev", "closed", true, true)
	report := pullRequestRunsFrom(head, github.CommitWorkflowRuns{HeadSHA: shadowCompareHead})
	if report.Runs == nil || len(report.Runs) != 0 || report.Gradable != 0 {
		t.Fatalf("report = %#v", report)
	}
	if !report.Merged || !report.FromFork {
		t.Fatalf("report lost the head's state: %#v", report)
	}
	encoded, err := json.Marshal(report)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if !strings.Contains(string(encoded), `"runs":[]`) {
		t.Fatalf("empty runs encoded as %s", encoded)
	}
}

// A fork head is reported, not hidden. Job policy distrusts fork pull
// requests, so an operator choosing representative ones has to be able to see
// which they are.
func TestAForkPullRequestIsReportedAsOne(t *testing.T) {
	head := github.PullRequestHead{
		Number: 12, HeadSHA: shadowCompareHead, BaseRef: "ra8ci/dev", State: "open",
		FromFork: true, HeadRepository: "someone/ra8-firmware",
	}
	report := pullRequestRunsFrom(head, github.CommitWorkflowRuns{
		HeadSHA: shadowCompareHead,
		Runs:    []github.CommitWorkflowRun{{ID: 7, Workflow: "checks", Attempt: 1, Status: "completed", Conclusion: "success"}},
	})
	if !report.FromFork || report.HeadRepository != "someone/ra8-firmware" || report.Gradable != 1 {
		t.Fatalf("report = %#v", report)
	}
}

// A document this command cannot read writes nothing and never reaches GitHub.
func TestAPullRequestAskIsRefusedBeforeAnyRead(t *testing.T) {
	cases := map[string]string{
		"not an object":     `["1589"]`,
		"unknown field":     `{"number":1589,"repository":"ra8-firmware"}`,
		"trailing document": `{"number":1589}{"number":1590}`,
		"no number":         `{}`,
		"zero":              `{"number":0}`,
		"negative":          `{"number":-1}`,
	}
	for name, document := range cases {
		t.Run(name, func(t *testing.T) {
			publishCheckRunEnv(t)
			var out strings.Builder
			if err := githubPullRequestRuns(context.Background(), strings.NewReader(document), &out); err == nil {
				t.Fatal("a document that cannot be read was accepted")
			}
			if out.Len() != 0 {
				t.Fatalf("refused ask still wrote %q", out.String())
			}
		})
	}
}

// The command needs the check-run configuration, like every other github
// subcommand that speaks to GitHub.
func TestPullRequestNeedsTheCheckRunConfiguration(t *testing.T) {
	for _, name := range []string{github.EnvShadowCorrespondenceFile, github.EnvCheckRunMode, github.EnvCheckRunRepository} {
		t.Setenv(name, "")
		os.Unsetenv(name)
	}
	err := githubPullRequestRuns(context.Background(), strings.NewReader(`{"number":1}`), io.Discard)
	if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
		t.Fatalf("error %v, want one naming %s", err, github.EnvShadowCorrespondenceFile)
	}
}

// The subcommand is reachable by name, and the usage line names it.
func TestPullRequestIsOneOfTheGithubSubcommands(t *testing.T) {
	err := githubCommand(context.Background(), []string{"pull-requests"})
	if err == nil {
		t.Fatal("a misspelled subcommand was accepted")
	}
	if !strings.Contains(err.Error(), "pull-request") {
		t.Fatalf("usage %q does not name the subcommand", err)
	}
}

// The selected run travels with enough of the pull request to judge whether it
// is representative evidence.
func TestTheEvidenceRunReportCarriesTheHeadAndTheRun(t *testing.T) {
	head := pullRequestHeadFor(1592, shadowCompareHead, "ra8ci/dev", "open", false, false)
	report := evidenceRunFrom(head, github.CommitWorkflowRun{
		ID: 909, Workflow: "Checks", Attempt: 2, Event: "pull_request",
		Status: "completed", Conclusion: "failure",
	})
	if report.RunID != 909 || report.Attempt != 2 || report.Conclusion != "failure" {
		t.Fatalf("report = %#v", report)
	}
	if report.Number != 1592 || report.HeadSHA != shadowCompareHead || report.BaseRef != "ra8ci/dev" {
		t.Fatalf("report lost the pull request: %#v", report)
	}
	encoded, err := json.Marshal(report)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	// The status is deliberately absent: a selected run has completed, so
	// reporting it would be a field with one value.
	if strings.Contains(string(encoded), `"status"`) {
		t.Fatalf("the report carries a status: %s", encoded)
	}
	if !strings.Contains(string(encoded), `"run_id":909`) {
		t.Fatalf("encoded as %s", encoded)
	}
}

// A fork pull request is reported as one here too: the run is only evidence if
// the pull request it came from is.
func TestAnEvidenceRunFromAForkSaysSo(t *testing.T) {
	head := github.PullRequestHead{
		Number: 12, HeadSHA: shadowCompareHead, BaseRef: "ra8ci/dev", State: "closed",
		Merged: true, FromFork: true, HeadRepository: "someone/ra8-firmware",
	}
	report := evidenceRunFrom(head, github.CommitWorkflowRun{
		ID: 7, Workflow: "Checks", Attempt: 1, Status: "completed", Conclusion: "success",
	})
	if !report.FromFork || !report.Merged || report.HeadRepository != "someone/ra8-firmware" {
		t.Fatalf("report = %#v", report)
	}
}

// A document this command cannot read writes nothing and never reaches GitHub.
func TestAnEvidenceRunAskIsRefusedBeforeAnyRead(t *testing.T) {
	cases := map[string]string{
		"not an object":     `["1592"]`,
		"unknown field":     `{"number":1592,"workflow":"Checks","run_id":9}`,
		"trailing document": `{"number":1592,"workflow":"Checks"}{"number":1593,"workflow":"Checks"}`,
		"no number":         `{"workflow":"Checks"}`,
		"zero number":       `{"number":0,"workflow":"Checks"}`,
		"negative number":   `{"number":-1,"workflow":"Checks"}`,
		"no workflow":       `{"number":1592}`,
		"blank workflow":    `{"number":1592,"workflow":"   "}`,
	}
	for name, document := range cases {
		t.Run(name, func(t *testing.T) {
			evidenceGateEnv(t)
			var out strings.Builder
			if err := githubEvidenceRun(context.Background(), strings.NewReader(document), &out); err == nil {
				t.Fatal("the ask was not refused")
			}
			if out.String() != "" {
				t.Fatalf("a refused ask wrote %q", out.String())
			}
		})
	}
}

func TestEvidenceRunNeedsTheCheckRunConfiguration(t *testing.T) {
	for _, name := range []string{github.EnvShadowCorrespondenceFile, github.EnvCheckRunMode, github.EnvCheckRunRepository} {
		t.Setenv(name, "")
		os.Unsetenv(name)
	}
	err := githubEvidenceRun(context.Background(), strings.NewReader(`{"number":1,"workflow":"Checks"}`), io.Discard)
	if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
		t.Fatalf("error %v, want one naming %s", err, github.EnvShadowCorrespondenceFile)
	}
}

func TestEvidenceRunIsOneOfTheGithubSubcommands(t *testing.T) {
	err := githubCommand(context.Background(), []string{"evidence-runs"})
	if err == nil {
		t.Fatal("an unknown subcommand was accepted")
	}
	if !strings.Contains(err.Error(), "evidence-run") {
		t.Fatalf("usage does not name the subcommand: %v", err)
	}
}

// pullRequestEvidenceHeadA and pullRequestEvidenceHeadB are the two commits
// the gathered document is pinned over. They are their own constants because
// the evidence document's whole job is to hold more than one commit.
const (
	pullRequestEvidenceHeadA = "1111111111111111111111111111111111111111"
	pullRequestEvidenceHeadB = "2222222222222222222222222222222222222222"
)

// gatheredFor builds one pull request's gathered half without contacting
// GitHub, so the document shape is pinned on its own.
func gatheredFor(number int, head string, runID int64, attempt int, task, job, conclusion string) gatheredPullRequest {
	return gatheredPullRequest{
		Number: number,
		Outcomes: github.ActionsRunOutcomes{
			RunID: runID, Attempt: attempt, HeadSHA: head,
			Outcomes: []github.ActionsOutcome{{Job: job, HeadSHA: head, Conclusion: conclusion}},
		},
		Plane: []planeOutcomeInput{{Task: task, HeadSHA: head, Observed: "failed"}},
	}
}

// The gathered document is exactly what shadow-evidence reads. It is decoded
// back with unknown fields refused, the way the command downstream reads it,
// so a field added here that shadow-evidence does not know breaks this test
// rather than the pipe.
func TestTheGatheredEvidenceIsWhatShadowEvidenceReads(t *testing.T) {
	document := pullRequestEvidenceDocument(2, []gatheredPullRequest{
		gatheredFor(1589, pullRequestEvidenceHeadA, 771, 1, "build", "build", "failure"),
		gatheredFor(1590, pullRequestEvidenceHeadB, 772, 2, "build", "build", "success"),
	})
	encoded, err := json.Marshal(document)
	if err != nil {
		t.Fatalf("encode the gathered document: %v", err)
	}
	var read shadowEvidenceInput
	decoder := json.NewDecoder(bytes.NewReader(encoded))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&read); err != nil {
		t.Fatalf("shadow-evidence would refuse the gathered document: %v", err)
	}
	if read.Threshold != 2 {
		t.Fatalf("threshold %d, want the stated 2", read.Threshold)
	}
	if len(read.Commits) != 2 {
		t.Fatalf("%d commits, want one per pull request", len(read.Commits))
	}
}

// Two pull requests at one commit are refused by name. The readiness
// threshold counts commits, so gathering both would put one commit into the
// evidence document twice and let it answer a threshold of two, which is the
// exact thing checkPullRequestEvidenceAsk refuses a repeated NUMBER for.
func TestTwoPullRequestsAtOneCommitAreRefusedByName(t *testing.T) {
	gatheredAt := make(gatheredHeads, 2)
	if err := gatheredAt.claim(1589, pullRequestEvidenceHeadA); err != nil {
		t.Fatalf("the first pull request was refused: %v", err)
	}
	err := gatheredAt.claim(1590, pullRequestEvidenceHeadA)
	if err == nil {
		t.Fatal("a second pull request at the same commit was gathered")
	}
	for _, want := range []string{"1589", "1590", pullRequestEvidenceHeadA} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal %q does not name %s", err, want)
		}
	}
}

// The refusal says why, not just that it happened. An operator holding it is
// choosing a representative set, and "one commit cannot answer for two" is
// the whole reason one of the two has to leave the candidate list.
func TestTheRefusalSaysWhyOneCommitCannotBeGatheredTwice(t *testing.T) {
	gatheredAt := make(gatheredHeads, 2)
	if err := gatheredAt.claim(1589, pullRequestEvidenceHeadA); err != nil {
		t.Fatalf("the first pull request was refused: %v", err)
	}
	err := gatheredAt.claim(1590, pullRequestEvidenceHeadA)
	if err == nil || !strings.Contains(err.Error(), "one commit cannot answer for two") {
		t.Fatalf("refusal %v does not say why", err)
	}
}

// GitHub's casing of a commit is not a different commit, the rule the rest of
// the package keeps (#1605, #1607). A candidate list assembled by hand is
// exactly where an upper case SHA turns up.
func TestTheSameCommitInADifferentCasingIsTheSameCommit(t *testing.T) {
	gatheredAt := make(gatheredHeads, 2)
	if err := gatheredAt.claim(1589, pullRequestEvidenceHeadA); err != nil {
		t.Fatalf("the first pull request was refused: %v", err)
	}
	if err := gatheredAt.claim(1590, strings.ToUpper(pullRequestEvidenceHeadA)); err == nil {
		t.Fatal("the same commit in upper case was gathered as a second commit")
	}
}

// Surrounding space is not a different commit either.
func TestACommitIsMatchedWithoutItsSurroundingSpace(t *testing.T) {
	gatheredAt := make(gatheredHeads, 2)
	if err := gatheredAt.claim(1589, pullRequestEvidenceHeadA); err != nil {
		t.Fatalf("the first pull request was refused: %v", err)
	}
	if err := gatheredAt.claim(1590, " "+pullRequestEvidenceHeadA+"\n"); err == nil {
		t.Fatal("the same commit with space around it was gathered as a second commit")
	}
}

// Pull requests at different commits are both gathered. The refusal is about
// one commit answering twice, not about how many pull requests there are.
func TestPullRequestsAtDifferentCommitsAreBothGathered(t *testing.T) {
	gatheredAt := make(gatheredHeads, 2)
	if err := gatheredAt.claim(1589, pullRequestEvidenceHeadA); err != nil {
		t.Fatalf("the first pull request was refused: %v", err)
	}
	if err := gatheredAt.claim(1590, pullRequestEvidenceHeadB); err != nil {
		t.Fatalf("a pull request at a different commit was refused: %v", err)
	}
	if len(gatheredAt) != 2 {
		t.Fatalf("%d commits claimed, want one per pull request", len(gatheredAt))
	}
}

// A head that is not a commit at all is refused here rather than carried into
// the document. The accumulation would refuse it downstream, but by then
// every remaining head and workflow run has been read for nothing.
func TestAPullRequestWithNoHeadCommitIsRefused(t *testing.T) {
	for name, head := range map[string]string{"empty": "", "space": "   "} {
		t.Run(name, func(t *testing.T) {
			err := make(gatheredHeads, 1).claim(1589, head)
			if err == nil || !strings.Contains(err.Error(), "1589") {
				t.Fatalf("error %v, want one naming the pull request", err)
			}
		})
	}
}

// The pull requests are gathered in the order they were asked for. The
// readiness answer counts commits, so the order changes nothing it decides,
// but a document that reordered them could not be read beside the ask it came
// from.
func TestTheGatheredCommitsKeepTheOrderTheyWereAskedIn(t *testing.T) {
	document := pullRequestEvidenceDocument(1, []gatheredPullRequest{
		gatheredFor(1590, pullRequestEvidenceHeadB, 772, 1, "build", "build", "success"),
		gatheredFor(1589, pullRequestEvidenceHeadA, 771, 1, "build", "build", "failure"),
	})
	if document.Commits[0].ActionsRun.HeadSHA != pullRequestEvidenceHeadB {
		t.Fatalf("first commit is %s, want the pull request asked for first",
			document.Commits[0].ActionsRun.HeadSHA)
	}
	if document.Commits[1].ActionsRun.HeadSHA != pullRequestEvidenceHeadA {
		t.Fatalf("second commit is %s", document.Commits[1].ActionsRun.HeadSHA)
	}
}

// Every gathered comparison names the run and attempt its Actions half was
// read from. A re-run answers the same run number with different conclusions,
// so evidence that does not name the attempt cannot be checked a second time.
func TestEveryGatheredCommitNamesTheRunItWasGradedFrom(t *testing.T) {
	document := pullRequestEvidenceDocument(1, []gatheredPullRequest{
		gatheredFor(1590, pullRequestEvidenceHeadB, 9001, 3, "build", "build", "success"),
	})
	anchor := document.Commits[0].ActionsRun
	if anchor == nil {
		t.Fatal("the gathered comparison names no workflow run")
	}
	if anchor.RunID != 9001 || anchor.Attempt != 3 || anchor.HeadSHA != pullRequestEvidenceHeadB {
		t.Fatalf("anchor %+v, want the run the outcomes were read from", *anchor)
	}
}

// The plane half is the caller's statement and passes through verbatim. This
// command reads the Actions half and never the plane's, the division
// actions-run draws, and a gatherer that edited the plane side would be making
// the claim the correspondence exists to keep explicit.
func TestTheGatheredPlaneHalfPassesThroughVerbatim(t *testing.T) {
	plane := []planeOutcomeInput{
		{Task: "build", HeadSHA: pullRequestEvidenceHeadA, Observed: "failed"},
		{Task: "lint", HeadSHA: pullRequestEvidenceHeadA, Observed: "succeeded"},
	}
	document := pullRequestEvidenceDocument(1, []gatheredPullRequest{{
		Number: 1589,
		Outcomes: github.ActionsRunOutcomes{
			RunID: 771, Attempt: 1, HeadSHA: pullRequestEvidenceHeadA,
			Outcomes: []github.ActionsOutcome{{Job: "build", HeadSHA: pullRequestEvidenceHeadA, Conclusion: "failure"}},
		},
		Plane: plane,
	}})
	if !reflect.DeepEqual(document.Commits[0].Plane, plane) {
		t.Fatalf("plane half %+v, want it verbatim", document.Commits[0].Plane)
	}
}

// A plane half stated for a commit the pull request is not at is refused, and
// the refusal names the pull request as well as both commits: the mismatch
// Collect reports downstream names only two SHAs, and this is the one place
// that knows which pull request they were gathered for.
func TestAPlaneHalfAboutAnotherCommitIsRefusedByPullRequest(t *testing.T) {
	asked := pullRequestEvidenceEntry{
		Number: 1589,
		Plane:  []planeOutcomeInput{{Task: "build", HeadSHA: pullRequestEvidenceHeadB, Observed: "failed"}},
	}
	err := checkPlaneIsAboutTheHead(asked, pullRequestEvidenceHeadA)
	if err == nil {
		t.Fatal("a plane half about another commit was accepted")
	}
	for _, want := range []string{"1589", pullRequestEvidenceHeadA, pullRequestEvidenceHeadB, "build"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal %q does not name %s", err, want)
		}
	}
}

// GitHub renders a commit in either case, and a pull request at the same
// commit written differently is the same commit.
func TestTheHeadCheckIsNotAboutTheCasingOfACommit(t *testing.T) {
	asked := pullRequestEvidenceEntry{
		Number: 1589,
		Plane:  []planeOutcomeInput{{Task: "build", HeadSHA: strings.ToUpper(pullRequestEvidenceHeadA), Observed: "failed"}},
	}
	if err := checkPlaneIsAboutTheHead(asked, pullRequestEvidenceHeadA); err != nil {
		t.Fatalf("the same commit in another casing was refused: %v", err)
	}
}

// An ask nothing could be gathered from is refused before a token is minted,
// and each refusal says which of them it is.
func TestAnUngatherableAskIsRefused(t *testing.T) {
	plane := []planeOutcomeInput{{Task: "build", HeadSHA: pullRequestEvidenceHeadA, Observed: "failed"}}
	one := pullRequestEvidenceEntry{Number: 1589, Plane: plane}
	refusals := map[string]struct {
		ask  pullRequestEvidenceRequest
		says string
	}{
		"no workflow": {pullRequestEvidenceRequest{
			Threshold: 1, PullRequests: []pullRequestEvidenceEntry{one}}, "workflow"},
		"a blank workflow": {pullRequestEvidenceRequest{
			Workflow: "   ", Threshold: 1, PullRequests: []pullRequestEvidenceEntry{one}}, "workflow"},
		"no threshold": {pullRequestEvidenceRequest{
			Workflow: "Checks", PullRequests: []pullRequestEvidenceEntry{one}}, "threshold"},
		"a negative threshold": {pullRequestEvidenceRequest{
			Workflow: "Checks", Threshold: -1, PullRequests: []pullRequestEvidenceEntry{one}}, "threshold"},
		"no pull requests": {pullRequestEvidenceRequest{
			Workflow: "Checks", Threshold: 1}, "pull requests"},
		"a pull request with no number": {pullRequestEvidenceRequest{
			Workflow: "Checks", Threshold: 1,
			PullRequests: []pullRequestEvidenceEntry{{Plane: plane}}}, "number"},
		"one pull request twice": {pullRequestEvidenceRequest{
			Workflow: "Checks", Threshold: 2,
			PullRequests: []pullRequestEvidenceEntry{one, one}}, "twice"},
		"a pull request with no plane half": {pullRequestEvidenceRequest{
			Workflow: "Checks", Threshold: 1,
			PullRequests: []pullRequestEvidenceEntry{{Number: 1589}}}, "plane"},
	}
	for name, refusal := range refusals {
		t.Run(name, func(t *testing.T) {
			err := checkPullRequestEvidenceAsk(refusal.ask)
			if err == nil {
				t.Fatal("the ask was accepted")
			}
			if !strings.Contains(err.Error(), refusal.says) {
				t.Fatalf("refusal %q does not say %q", err, refusal.says)
			}
		})
	}
}

// Two pull requests are gathered, and the same pull request twice is not: the
// threshold counts commits, so a repeated number would let one pull request
// answer a threshold of two.
func TestTwoDifferentPullRequestsAreGathered(t *testing.T) {
	plane := []planeOutcomeInput{{Task: "build", HeadSHA: pullRequestEvidenceHeadA, Observed: "failed"}}
	ask := pullRequestEvidenceRequest{Workflow: "Checks", Threshold: 2, PullRequests: []pullRequestEvidenceEntry{
		{Number: 1589, Plane: plane}, {Number: 1590, Plane: plane},
	}}
	if err := checkPullRequestEvidenceAsk(ask); err != nil {
		t.Fatalf("two pull requests were refused: %v", err)
	}
}

// A refused document writes nothing: half a gathered evidence document reads
// as evidence over a smaller set rather than as a read that did not happen.
func TestAPullRequestEvidenceAskThatIsRefusedWritesNothing(t *testing.T) {
	publishCheckRunEnv(t)
	refusals := map[string]string{
		"not an object":     `["1589"]`,
		"unknown field":     `{"workflow":"Checks","threshold":1,"commits":[]}`,
		"trailing document": `{"workflow":"Checks","threshold":1,"pull_requests":[]} {}`,
		"no workflow":       `{"threshold":1,"pull_requests":[{"number":1589,"plane":[]}]}`,
		"no threshold":      `{"workflow":"Checks","pull_requests":[{"number":1589,"plane":[]}]}`,
		"no pull requests":  `{"workflow":"Checks","threshold":1,"pull_requests":[]}`,
	}
	for name, document := range refusals {
		t.Run(name, func(t *testing.T) {
			var out strings.Builder
			if err := githubPullRequestEvidence(context.Background(), strings.NewReader(document), &out); err == nil {
				t.Fatal("the document was accepted")
			}
			if out.Len() != 0 {
				t.Fatalf("wrote %q", out.String())
			}
		})
	}
}

// Gathering needs the same configuration the other check-run commands do, and
// says which half is missing.
func TestPullRequestEvidenceNeedsTheCheckRunConfiguration(t *testing.T) {
	for _, name := range []string{github.EnvShadowCorrespondenceFile, github.EnvCheckRunMode, github.EnvCheckRunRepository} {
		t.Setenv(name, "")
		os.Unsetenv(name)
	}
	err := githubPullRequestEvidence(context.Background(), strings.NewReader("{}"), io.Discard)
	if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
		t.Fatalf("error %v, want one naming %s", err, github.EnvShadowCorrespondenceFile)
	}
}

// The subcommand is reachable by name, and the usage line names it.
func TestPullRequestEvidenceIsOneOfTheGithubSubcommands(t *testing.T) {
	err := githubCommand(context.Background(), []string{"pull-request-evidenc"})
	if err == nil {
		t.Fatal("a misspelled subcommand was accepted")
	}
	if !strings.Contains(err.Error(), "pull-request-evidence") {
		t.Fatalf("usage %q does not name the subcommand", err)
	}
}

// surveyHeadA and surveyHeadB are the two commits the survey report is pinned
// over. They are their own constants because the other test files in this
// package each keep their own head and reusing one across them couples tests
// that answer different questions.
const (
	surveyHeadA = "3333333333333333333333333333333333333333"
	surveyHeadB = "4444444444444444444444444444444444444444"
)

// selectableFor is one pull request the survey could pick a run on.
func selectableFor(number int, head string, runID int64, attempt int, conclusion string) surveyedHead {
	return surveyedHead{
		Head: pullRequestHeadFor(number, head, "ra8ci/dev", "open", false, false),
		Selected: github.CommitWorkflowRun{
			ID: runID, Workflow: "Checks", Attempt: attempt, Event: "pull_request",
			Status: "completed", Conclusion: conclusion,
		},
	}
}

// unselectableFor is one pull request the selection refused, carrying the
// refusal github.SelectEvidenceRun would actually return.
func unselectableFor(number int, head string, listed github.CommitWorkflowRuns) surveyedHead {
	_, refusal := github.SelectEvidenceRun(listed, "Checks")
	return surveyedHead{
		Head:    pullRequestHeadFor(number, head, "ra8ci/dev", "open", false, false),
		Refusal: refusal,
	}
}

// The survey answers for every pull request it was asked about, including the
// ones no run can be selected on. Dropping them would hide from the operator
// picking a representative set exactly the candidates that cannot be in it.
func TestTheSurveyAnswersForEveryPullRequestConsidered(t *testing.T) {
	report := pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
		unselectableFor(1590, surveyHeadB, github.CommitWorkflowRuns{
			HeadSHA: surveyHeadB,
			Runs: []github.CommitWorkflowRun{{
				ID: 772, Workflow: "Checks", Attempt: 1, Event: "pull_request",
				Status: "in_progress",
			}},
		}),
	})
	if report.Considered != 2 || report.Selectable != 1 || report.Unselectable != 1 {
		t.Fatalf("counts = %#v", report)
	}
	if len(report.PullRequests) != 2 {
		t.Fatalf("report holds %d pull requests", len(report.PullRequests))
	}
	if report.PullRequests[0].Number != 1589 || report.PullRequests[1].Number != 1590 {
		t.Fatalf("the survey reordered the candidates: %#v", report.PullRequests)
	}
	if report.Workflow != "Checks" {
		t.Fatalf("the report lost the workflow: %q", report.Workflow)
	}
}

// The survey names the commits more than one candidate is at. The gather
// refuses that pair, because the readiness threshold counts commits, and
// until the survey says so an operator finds out only after every head and
// workflow run in the set has been read.
func TestTheSurveyNamesTheCommitTwoCandidatesShare(t *testing.T) {
	report := pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
		selectableFor(1590, surveyHeadA, 771, 1, "failure"),
		selectableFor(1591, surveyHeadB, 772, 1, "success"),
	})
	if len(report.SharedHeads) != 1 {
		t.Fatalf("shared heads = %#v, want the one commit two candidates are at", report.SharedHeads)
	}
	shared := report.SharedHeads[0]
	if shared.HeadSHA != surveyHeadA {
		t.Fatalf("shared head %s, want %s", shared.HeadSHA, surveyHeadA)
	}
	if len(shared.PullRequests) != 2 || shared.PullRequests[0] != 1589 || shared.PullRequests[1] != 1590 {
		t.Fatalf("shared by %v, want 1589 and 1590 in the order asked", shared.PullRequests)
	}
}

// The candidates at their own commits are not named. A report that listed
// every head would say nothing, and the line exists to be read only when
// there is something wrong with the set.
func TestAnOrdinarySurveyNamesNoSharedCommit(t *testing.T) {
	report := pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
		selectableFor(1590, surveyHeadB, 772, 1, "success"),
	})
	if len(report.SharedHeads) != 0 {
		t.Fatalf("shared heads = %#v, want none", report.SharedHeads)
	}
}

// An empty shared-head list encodes as [] and never as null, the convention
// every other list in these reports keeps.
func TestNoSharedCommitsEncodeAsAnEmptyList(t *testing.T) {
	encoded, err := json.Marshal(pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
	}))
	if err != nil {
		t.Fatalf("encode the survey: %v", err)
	}
	if !strings.Contains(string(encoded), `"shared_heads": []`) &&
		!strings.Contains(string(encoded), `"shared_heads":[]`) {
		t.Fatalf("shared heads did not encode as an empty list: %s", encoded)
	}
}

// The counts are left alone. Considered and Selectable are true of each pull
// request on its own, and rewriting them for a shared commit would answer a
// different question than the one they have always answered.
func TestASharedCommitDoesNotMoveTheSurveyCounts(t *testing.T) {
	report := pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
		selectableFor(1590, surveyHeadA, 771, 1, "failure"),
	})
	if report.Considered != 2 || report.Selectable != 2 || report.Unselectable != 0 {
		t.Fatalf("counts = %#v", report)
	}
	if len(report.PullRequests) != 2 {
		t.Fatalf("the survey dropped a candidate: %#v", report.PullRequests)
	}
}

// Three candidates at one commit are named together, not as two pairs.
func TestThreeCandidatesAtOneCommitAreNamedOnce(t *testing.T) {
	report := pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
		selectableFor(1590, surveyHeadA, 771, 1, "failure"),
		selectableFor(1591, surveyHeadA, 771, 1, "failure"),
	})
	if len(report.SharedHeads) != 1 {
		t.Fatalf("shared heads = %#v, want one entry", report.SharedHeads)
	}
	if got := report.SharedHeads[0].PullRequests; len(got) != 3 {
		t.Fatalf("shared by %v, want all three", got)
	}
}

// GitHub's casing of a commit is not a different commit, the rule the gather
// keeps too: a candidate list assembled by hand is where an upper case SHA
// turns up, and it must not read as two separate commits here.
func TestASharedCommitIsFoundWhateverItsCasing(t *testing.T) {
	report := pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
		selectableFor(1590, strings.ToUpper(surveyHeadA), 771, 1, "failure"),
	})
	if len(report.SharedHeads) != 1 {
		t.Fatalf("shared heads = %#v, want the commit found across casings", report.SharedHeads)
	}
	if report.SharedHeads[0].HeadSHA != surveyHeadA {
		t.Fatalf("head %s, want it as the first candidate stated it", report.SharedHeads[0].HeadSHA)
	}
}

// Two pull requests with no head commit are two unanswered heads, not a
// shared commit. Grouping them would point an operator at a clash that is
// not there.
func TestCandidatesWithNoHeadCommitAreNotAShare(t *testing.T) {
	report := pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, "", 771, 1, "failure"),
		selectableFor(1590, "", 772, 1, "failure"),
	})
	if len(report.SharedHeads) != 0 {
		t.Fatalf("shared heads = %#v, want none", report.SharedHeads)
	}
}

// The shared commits are named in the order they were first surveyed, so the
// report can be read beside the ask it came from.
func TestSharedCommitsKeepTheOrderTheyWereSurveyedIn(t *testing.T) {
	report := pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadB, 772, 1, "success"),
		selectableFor(1590, surveyHeadA, 771, 1, "failure"),
		selectableFor(1591, surveyHeadB, 772, 1, "success"),
		selectableFor(1592, surveyHeadA, 771, 1, "failure"),
	})
	if len(report.SharedHeads) != 2 {
		t.Fatalf("shared heads = %#v, want both commits", report.SharedHeads)
	}
	if report.SharedHeads[0].HeadSHA != surveyHeadB || report.SharedHeads[1].HeadSHA != surveyHeadA {
		t.Fatalf("shared heads were reordered: %#v", report.SharedHeads)
	}
}

// A selectable pull request names the run the gathering would use.
func TestASelectablePullRequestNamesItsRun(t *testing.T) {
	report := pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 909, 2, "failure"),
	})
	answer := report.PullRequests[0]
	if !answer.Selectable {
		t.Fatalf("answer = %#v", answer)
	}
	if answer.RunID != 909 || answer.Attempt != 2 || answer.Conclusion != "failure" {
		t.Fatalf("answer lost the run: %#v", answer)
	}
	if answer.Event != "pull_request" {
		t.Fatalf("answer lost the event: %#v", answer)
	}
	if answer.Reason != "" {
		t.Fatalf("a selectable pull request carries a reason: %q", answer.Reason)
	}
	if answer.HeadSHA != surveyHeadA || answer.BaseRef != "ra8ci/dev" {
		t.Fatalf("answer lost the head: %#v", answer)
	}
}

// A pull request no run can be selected on carries the refusal in words and
// no run at all: reporting the run fields of a refused selection would hand a
// reader a run ID the gathering will not use.
func TestAnUnselectablePullRequestCarriesTheReasonAndNoRun(t *testing.T) {
	cases := map[string]github.CommitWorkflowRuns{
		"no runs at all": {HeadSHA: surveyHeadB},
		"only another workflow": {HeadSHA: surveyHeadB, Runs: []github.CommitWorkflowRun{{
			ID: 1, Workflow: "Docs", Attempt: 1, Status: "completed", Conclusion: "success",
		}}},
		"still executing": {HeadSHA: surveyHeadB, Runs: []github.CommitWorkflowRun{{
			ID: 2, Workflow: "Checks", Attempt: 1, Status: "in_progress",
		}}},
		"concluded without deciding": {HeadSHA: surveyHeadB, Runs: []github.CommitWorkflowRun{{
			ID: 3, Workflow: "Checks", Attempt: 1, Status: "completed", Conclusion: "cancelled",
		}}},
		"two decided runs": {HeadSHA: surveyHeadB, Runs: []github.CommitWorkflowRun{
			{ID: 4, Workflow: "Checks", Attempt: 1, Status: "completed", Conclusion: "success"},
			{ID: 5, Workflow: "Checks", Attempt: 2, Status: "completed", Conclusion: "success"},
		}},
	}
	for name, listed := range cases {
		t.Run(name, func(t *testing.T) {
			report := pullRequestSurveyFrom("Checks", []surveyedHead{
				unselectableFor(1590, surveyHeadB, listed),
			})
			answer := report.PullRequests[0]
			if answer.Selectable {
				t.Fatalf("answer = %#v", answer)
			}
			if answer.Reason == "" {
				t.Fatal("an unselectable pull request gave no reason")
			}
			if answer.RunID != 0 || answer.Attempt != 0 || answer.Conclusion != "" || answer.Event != "" {
				t.Fatalf("a refused selection carried a run: %#v", answer)
			}
			if report.Selectable != 0 || report.Unselectable != 1 {
				t.Fatalf("counts = %#v", report)
			}
		})
	}
}

// The head's state travels with every answer, selectable or not: whether a
// fork or a merged pull request is representative is the operator's call, and
// they are choosing the set from this report.
func TestTheSurveyReportsWhereEachPullRequestIs(t *testing.T) {
	_, refusal := github.SelectEvidenceRun(github.CommitWorkflowRuns{HeadSHA: surveyHeadB}, "Checks")
	report := pullRequestSurveyFrom("Checks", []surveyedHead{{
		Head: github.PullRequestHead{
			Number: 12, HeadSHA: surveyHeadB, BaseRef: "ra8ci/dev", State: "closed",
			Merged: true, FromFork: true, HeadRepository: "someone/ra8-firmware",
		},
		Refusal: refusal,
	}})
	answer := report.PullRequests[0]
	if !answer.FromFork || !answer.Merged || answer.State != "closed" {
		t.Fatalf("answer = %#v", answer)
	}
	if answer.HeadRepository != "someone/ra8-firmware" {
		t.Fatalf("answer lost the head repository: %#v", answer)
	}
}

// An empty survey encodes as an empty list rather than null, so two reports
// diff against each other.
func TestASurveyOfNothingEncodesAsAnEmptyList(t *testing.T) {
	encoded, err := json.Marshal(pullRequestSurveyFrom("Checks", nil))
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if !strings.Contains(string(encoded), `"pull_requests":[]`) {
		t.Fatalf("encoded as %s", encoded)
	}
	if !strings.Contains(string(encoded), `"considered":0`) {
		t.Fatalf("encoded as %s", encoded)
	}
}

// The run named here is the run the gathering would select. Both ask
// github.SelectEvidenceRun, so a survey cannot promise a run
// pull-request-evidence would not use.
func TestTheSurveyNamesTheRunTheGatheringWouldSelect(t *testing.T) {
	listed := github.CommitWorkflowRuns{
		HeadSHA: surveyHeadA,
		Runs: []github.CommitWorkflowRun{
			{ID: 900, Workflow: "Docs", Attempt: 1, Status: "completed", Conclusion: "success"},
			{ID: 901, Workflow: "Checks", Attempt: 3, Event: "pull_request", Status: "completed", Conclusion: "failure"},
		},
	}
	selected, err := github.SelectEvidenceRun(listed, "Checks")
	if err != nil {
		t.Fatalf("select: %v", err)
	}
	report := pullRequestSurveyFrom("Checks", []surveyedHead{{
		Head:     pullRequestHeadFor(1589, surveyHeadA, "ra8ci/dev", "open", false, false),
		Selected: selected,
	}})
	if report.PullRequests[0].RunID != 901 || report.PullRequests[0].Attempt != 3 {
		t.Fatalf("answer = %#v", report.PullRequests[0])
	}
}

// An ask nothing could be surveyed from is refused before a token is minted,
// and writes nothing.
func TestASurveyAskThatIsRefusedWritesNothing(t *testing.T) {
	cases := map[string]string{
		"not an object":     `[1589]`,
		"unknown field":     `{"workflow":"Checks","pull_requests":[1589],"threshold":2}`,
		"trailing document": `{"workflow":"Checks","pull_requests":[1589]}{"workflow":"Checks","pull_requests":[1590]}`,
		"no workflow":       `{"pull_requests":[1589]}`,
		"blank workflow":    `{"workflow":"  ","pull_requests":[1589]}`,
		"no pull requests":  `{"workflow":"Checks","pull_requests":[]}`,
		"a zero number":     `{"workflow":"Checks","pull_requests":[0]}`,
		"a negative number": `{"workflow":"Checks","pull_requests":[-1]}`,
		"one twice":         `{"workflow":"Checks","pull_requests":[1589,1589]}`,
	}
	for name, document := range cases {
		t.Run(name, func(t *testing.T) {
			evidenceGateEnv(t)
			var out strings.Builder
			if err := githubPullRequestSurvey(context.Background(), strings.NewReader(document), &out); err == nil {
				t.Fatal("the ask was not refused")
			}
			if out.String() != "" {
				t.Fatalf("a refused ask wrote %q", out.String())
			}
		})
	}
}

// A pull request named twice is refused by name, so the operator is told what
// to fix rather than shown a count that answered for it twice.
func TestASurveyRefusesARepeatedPullRequestByName(t *testing.T) {
	err := checkPullRequestSurveyAsk(pullRequestSurveyRequest{
		Workflow: "Checks", PullRequests: []int{1589, 1590, 1589},
	})
	if err == nil {
		t.Fatal("a repeated pull request was accepted")
	}
	for _, want := range []string{"1589", "twice"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal %q does not name %q", err, want)
		}
	}
}

// Two different pull requests are surveyed.
func TestTwoDifferentPullRequestsAreSurveyed(t *testing.T) {
	if err := checkPullRequestSurveyAsk(pullRequestSurveyRequest{
		Workflow: "Checks", PullRequests: []int{1589, 1590},
	}); err != nil {
		t.Fatalf("a two pull request survey was refused: %v", err)
	}
}

func TestPullRequestSurveyNeedsTheCheckRunConfiguration(t *testing.T) {
	for _, name := range []string{github.EnvShadowCorrespondenceFile, github.EnvCheckRunMode, github.EnvCheckRunRepository} {
		t.Setenv(name, "")
		os.Unsetenv(name)
	}
	err := githubPullRequestSurvey(context.Background(),
		strings.NewReader(`{"workflow":"Checks","pull_requests":[1589]}`), io.Discard)
	if err == nil || !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
		t.Fatalf("error %v, want one naming %s", err, github.EnvShadowCorrespondenceFile)
	}
}

func TestPullRequestSurveyIsOneOfTheGithubSubcommands(t *testing.T) {
	err := githubCommand(context.Background(), []string{"pull-request-surveys"})
	if err == nil {
		t.Fatal("an unknown subcommand was accepted")
	}
	if !strings.Contains(err.Error(), "pull-request-survey") {
		t.Fatalf("usage does not name the subcommand: %v", err)
	}
}

// The survey carries each published run's title and summary, because the
// decision beside them deliberately does not read the summary: a run whose
// title agrees while its summary describes other work is settled as far as
// the reconciliation is concerned, and the report is where a person sees it.
func TestTheSurveyReportsWhatEachRunSaid(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	run := publishedAs(plan, 90, "completed", plan.Run.Conclusion, plan.Run.Title)
	run.Summary = "flash write timed out on board ra8d2-07"

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(run))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	if report.Tasks[0].Decision != "settled" {
		t.Fatalf("decision = %q, want settled", report.Tasks[0].Decision)
	}
	published := report.Tasks[0].Published
	if len(published) != 1 {
		t.Fatalf("published = %d, want 1", len(published))
	}
	if published[0].Title != plan.Run.Title {
		t.Fatalf("title = %q, want %q", published[0].Title, plan.Run.Title)
	}
	if published[0].Summary != "flash write timed out on board ra8d2-07" || published[0].SummaryTruncated {
		t.Fatalf("summary = %q truncated = %v", published[0].Summary, published[0].SummaryTruncated)
	}
}

// A summary longer than the report carries is cut, and the cut says so. An
// excerpt that did not announce itself would be read as the run's own words
// when it is only their beginning.
func TestALongSummaryIsExcerptedAndSaysSo(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	run := publishedAs(plan, 91, "completed", plan.Run.Conclusion, plan.Run.Title)
	run.Summary = strings.Repeat("a", maxReportedCheckRunSummary+50)

	report, err := surveyCheckRunPlan([]plannedCheckRun{plan}, listing(run))
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	reported := report.Tasks[0].Published[0]
	if len([]rune(reported.Summary)) != maxReportedCheckRunSummary {
		t.Fatalf("summary length = %d, want %d", len([]rune(reported.Summary)), maxReportedCheckRunSummary)
	}
	if !reported.SummaryTruncated {
		t.Fatal("a cut summary did not say it was cut")
	}
}

// The cut is by rune. Halving a multi-byte character would put a replacement
// character into a document meant to be the run's own words as far as it goes.
func TestASummaryIsCutOnACharacterBoundary(t *testing.T) {
	body := strings.Repeat("\u00e9", maxReportedCheckRunSummary+10)
	excerpt, truncated := excerptCheckRunSummary(body)
	if !truncated {
		t.Fatal("an over-long summary was not reported as cut")
	}
	if !utf8.ValidString(excerpt) {
		t.Fatalf("excerpt is not valid UTF-8: %q", excerpt)
	}
	if excerpt != strings.Repeat("\u00e9", maxReportedCheckRunSummary) {
		t.Fatalf("excerpt = %q", excerpt)
	}
}

// An exact-length summary is whole, not cut. The bound is a ceiling, not a
// point the report starts lying at.
func TestASummaryAtTheBoundIsNotCut(t *testing.T) {
	body := strings.Repeat("b", maxReportedCheckRunSummary)
	excerpt, truncated := excerptCheckRunSummary(body)
	if truncated || excerpt != body {
		t.Fatalf("excerpt = %d runes truncated = %v, want the whole summary", len([]rune(excerpt)), truncated)
	}
}

// A run with no output body is reported with an empty summary and no cut,
// which is a different thing from an excerpt that came out empty.
func TestARunWithNoSummaryReportsNoCut(t *testing.T) {
	excerpt, truncated := excerptCheckRunSummary("")
	if excerpt != "" || truncated {
		t.Fatalf("excerpt = %q truncated = %v", excerpt, truncated)
	}
}

// An insufficient task is reported with how far short it is and with the
// commits nobody judged, because those are the pull requests an operator goes
// back to. A name and a count send them through the whole document by hand.
func TestShadowEvidenceNamesTheUnjudgedCommitsAndTheShortfall(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	var out bytes.Buffer
	err := githubShadowEvidence(strings.NewReader(shadowEvidenceInputDocument(3,
		shadowEvidenceCommit(task, evidenceHeadOne, "success", "success"),
		shadowEvidenceCommit(task, evidenceHeadTwo, "success", ""),
		shadowEvidenceCommit(task, evidenceHeadThree, "success", ""))), &out)
	if err == nil {
		t.Fatal("unsettled evidence exited clean")
	}
	report := decodeShadowEvidence(t, out.String())

	tasks, _ := report["tasks"].([]any)
	if len(tasks) != 1 {
		t.Fatalf("tasks %v, want one", report["tasks"])
	}
	first, _ := tasks[0].(map[string]any)
	ungraded, _ := first["indeterminate_commits"].([]any)
	if len(ungraded) != 2 {
		t.Fatalf("indeterminate_commits %v, want two", first["indeterminate_commits"])
	}
	if ungraded[0] != evidenceHeadTwo || ungraded[1] != evidenceHeadThree {
		t.Fatalf("indeterminate_commits %v, want %s then %s",
			ungraded, evidenceHeadTwo, evidenceHeadThree)
	}

	shortfall, _ := report["shortfall"].([]any)
	if len(shortfall) != 1 {
		t.Fatalf("shortfall %v, want one entry", report["shortfall"])
	}
	entry, _ := shortfall[0].(map[string]any)
	if entry["task"] != task {
		t.Fatalf("shortfall names %v, want %s", entry["task"], task)
	}
	if entry["graded"] != float64(1) || entry["remaining"] != float64(2) {
		t.Fatalf("shortfall %v, want graded 1 remaining 2", entry)
	}
	if strings.Contains(out.String(), "null") {
		t.Fatalf("report carries a null field:\n%s", out.String())
	}
}

// Settled evidence writes both new fields as empty lists rather than leaving
// a reader to tell a missing field from an answer of none.
func TestSettledShadowEvidenceWritesEmptyUnjudgedAndShortfallLists(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	var out bytes.Buffer
	if err := githubShadowEvidence(strings.NewReader(shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(task, evidenceHeadOne, "success", "success"))), &out); err != nil {
		t.Fatalf("githubShadowEvidence: %v", err)
	}
	report := decodeShadowEvidence(t, out.String())
	shortfall, ok := report["shortfall"].([]any)
	if !ok {
		t.Fatalf("shortfall %v is not a list", report["shortfall"])
	}
	if len(shortfall) != 0 {
		t.Fatalf("shortfall %v, want none on settled evidence", shortfall)
	}
	tasks, _ := report["tasks"].([]any)
	first, _ := tasks[0].(map[string]any)
	ungraded, ok := first["indeterminate_commits"].([]any)
	if !ok {
		t.Fatalf("indeterminate_commits %v is not a list", first["indeterminate_commits"])
	}
	if len(ungraded) != 0 {
		t.Fatalf("indeterminate_commits %v, want none", ungraded)
	}
}

// The page and the document are the same answer written two ways. An operator
// reading a page that says a task is ready while the gate reads a document
// saying it is not has been given two facts and no way to tell which the
// decision was made on.
func TestTheEvidencePageAndTheEvidenceDocumentAgree(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	document := shadowEvidenceInputDocument(2,
		shadowEvidenceCommit(task, evidenceHeadOne, "success", "success"),
		shadowEvidenceCommit(task, evidenceHeadTwo, "success", "success"))

	var asDocument, asPage bytes.Buffer
	if err := githubShadowEvidence(strings.NewReader(document), &asDocument); err != nil {
		t.Fatalf("settled evidence returned %v", err)
	}
	if err := githubEvidencePage(strings.NewReader(document), &asPage); err != nil {
		t.Fatalf("settled evidence page returned %v", err)
	}
	report := decodeShadowEvidence(t, asDocument.String())
	if report["settled"] != true {
		t.Fatalf("settled %v, want true", report["settled"])
	}
	page := asPage.String()
	if !strings.Contains(page, "every compared task is ready") {
		t.Fatalf("the page does not agree with the document:\n%s", page)
	}
	if !strings.Contains(page, "threshold 2") || !strings.Contains(page, task) {
		t.Fatalf("the page does not name what it answered for:\n%s", page)
	}
}

// The page is written before the verdict is returned, the same rule the
// document writer follows: a caller reading only the exit status must not be
// able to get a settled one from an answer nobody could read.
func TestTheEvidencePageIsWrittenBeforeReportingItIsUnsettled(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	var out bytes.Buffer
	err := githubEvidencePage(strings.NewReader(shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(task, evidenceHeadOne, "failure", "success"))), &out)
	if err == nil {
		t.Fatal("a conflict returned no error")
	}
	page := out.String()
	if !strings.Contains(page, "holds the required checks: 1 disagreed with Actions") {
		t.Fatalf("the page does not state the hold:\n%s", page)
	}
	if !strings.Contains(page, "disagreed on: "+evidenceHeadOne) {
		t.Fatalf("the page does not name the commit to go back to:\n%s", page)
	}
}

// A task short of the threshold is chased through the pull requests it was
// never judged on, so the page has to name them and say how far short it is.
func TestTheEvidencePageNamesTheShortfallAndItsCommits(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	var out bytes.Buffer
	err := githubEvidencePage(strings.NewReader(shadowEvidenceInputDocument(3,
		shadowEvidenceCommit(task, evidenceHeadOne, "success", "success"),
		shadowEvidenceCommit(task, evidenceHeadTwo, "success", ""))), &out)
	if err == nil {
		t.Fatal("an unsettled page returned no error")
	}
	page := out.String()
	if !strings.Contains(page, "graded on 1, 2 more needed") {
		t.Fatalf("the shortfall is not on the page:\n%s", page)
	}
	if !strings.Contains(page, "never judged on: "+evidenceHeadTwo) {
		t.Fatalf("the unjudged commit is not named:\n%s", page)
	}
}

// A task the commit did not exercise is not a pairing and must not read as
// one, the githubShadowCompare convention: it is named after the page.
func TestTheEvidencePageNamesWhatACommitDidNotExercise(t *testing.T) {
	exercised, skipped := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t,
		`{"`+exercised+`":"build","`+skipped+`":"lint"}`))

	var out bytes.Buffer
	// The evidence settles: a task no commit exercised is not evidence for
	// that task and is not accumulated as any, so it does not hold the
	// answer. It is still named, because a task that reads as
	// under-observed has to be explainable without going back to the
	// inputs.
	if err := githubEvidencePage(strings.NewReader(shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(exercised, evidenceHeadOne, "success", "success"))), &out); err != nil {
		t.Fatalf("settled evidence page returned %v", err)
	}
	page := out.String()
	if !strings.Contains(page, "not exercised on "+evidenceHeadOne+": "+skipped) {
		t.Fatalf("the unexercised task is not named:\n%s", page)
	}
	// It is named AFTER the page, never as a pairing inside it.
	if strings.Index(page, "not exercised on") < strings.Index(page, "ready (may move)") {
		t.Fatalf("the unexercised task is folded into the pairings:\n%s", page)
	}
}

// Every commit that skipped a task is named, not just the first. The evidence
// is plural by construction and a reader working out why a task reads as
// under-observed needs the whole list, not a sample of it.
func TestTheEvidencePageNamesEveryCommitThatSkippedATask(t *testing.T) {
	exercised, skipped := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t,
		`{"`+exercised+`":"build","`+skipped+`":"lint"}`))

	var out bytes.Buffer
	if err := githubEvidencePage(strings.NewReader(shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(exercised, evidenceHeadOne, "success", "success"),
		shadowEvidenceCommit(exercised, evidenceHeadTwo, "success", "success"))), &out); err != nil {
		t.Fatalf("settled evidence page returned %v", err)
	}
	page := out.String()
	for _, head := range []string{evidenceHeadOne, evidenceHeadTwo} {
		if !strings.Contains(page, "not exercised on "+head+": "+skipped) {
			t.Fatalf("%s is not named as having skipped %s:\n%s", head, skipped, page)
		}
	}
}

// The document and the page are written from one per-commit record, so they
// cannot disagree about what a commit exercised. They used to share it as a
// map whose keys the page recovered with an unchecked assertion: a renamed
// key left the document intact and dropped the whole section off the page,
// which is the one failure that makes thin evidence look broad.
func TestTheDocumentAndThePageAgreeOnWhatEachCommitSkipped(t *testing.T) {
	exercised, skipped := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t,
		`{"`+exercised+`":"build","`+skipped+`":"lint"}`))
	input := shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(exercised, evidenceHeadOne, "success", "success"),
		shadowEvidenceCommit(exercised, evidenceHeadTwo, "success", "success"))

	var document bytes.Buffer
	if err := githubShadowEvidence(strings.NewReader(input), &document); err != nil {
		t.Fatalf("githubShadowEvidence: %v", err)
	}
	var page bytes.Buffer
	if err := githubEvidencePage(strings.NewReader(input), &page); err != nil {
		t.Fatalf("githubEvidencePage: %v", err)
	}

	report := decodeShadowEvidence(t, document.String())
	commits, _ := report["commits"].([]any)
	if len(commits) != 2 {
		t.Fatalf("commits %v, want two", report["commits"])
	}
	named := 0
	for _, entry := range commits {
		commit, _ := entry.(map[string]any)
		head, _ := commit["head_sha"].(string)
		names, _ := commit["not_exercised"].([]any)
		if _, ok := commit["comparisons"].(float64); !ok {
			t.Fatalf("comparisons %v on %s is not a number", commit["comparisons"], head)
		}
		if len(names) == 0 {
			if strings.Contains(page.String(), "not exercised on "+head) {
				t.Fatalf("%s skipped nothing in the document and is named on the page:\n%s",
					head, page.String())
			}
			continue
		}
		named++
		for _, name := range names {
			task, _ := name.(string)
			if !strings.Contains(page.String(), "not exercised on "+head+": "+task) {
				t.Fatalf("%s skipped %s in the document and the page does not say so:\n%s",
					head, task, page.String())
			}
		}
	}
	if named == 0 {
		t.Fatal("no commit skipped a task, so the two writers were never compared")
	}
}

// A task no accumulated commit paired carries no evidence at all, so it is in
// none of ready/conflicting/insufficient and the answer can settle without it
// ever having been compared. That is the mistake the accumulation opens by
// naming, and it is stated once rather than once per commit.
func TestTheAnswerNamesACoveredTaskNoCommitExercised(t *testing.T) {
	exercised, skipped := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t,
		`{"`+exercised+`":"build","`+skipped+`":"lint"}`))
	input := shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(exercised, evidenceHeadOne, "success", "success"),
		shadowEvidenceCommit(exercised, evidenceHeadTwo, "success", "success"))

	var document bytes.Buffer
	if err := githubShadowEvidence(strings.NewReader(input), &document); err != nil {
		t.Fatalf("githubShadowEvidence: %v", err)
	}
	report := decodeShadowEvidence(t, document.String())
	// The answer settles: the task it never compared is not evidence
	// against anything, and this command does not fail over it.
	if report["settled"] != true {
		t.Fatalf("settled %v, want true", report["settled"])
	}
	never, _ := report["never_exercised"].([]any)
	if len(never) != 1 || never[0] != skipped {
		t.Fatalf("never_exercised %v, want [%s]", report["never_exercised"], skipped)
	}

	var page bytes.Buffer
	if err := githubEvidencePage(strings.NewReader(input), &page); err != nil {
		t.Fatalf("githubEvidencePage: %v", err)
	}
	if !strings.Contains(page.String(), "never exercised on any commit: "+skipped) {
		t.Fatalf("the never-exercised task is not named on the page:\n%s", page.String())
	}
	// Once, as a set. The per-commit lines carry the same fact and the
	// summary is not one of them.
	if got := strings.Count(page.String(), "never exercised on any commit"); got != 1 {
		t.Fatalf("the summary appears %d times, want once:\n%s", got, page.String())
	}
}

// A task exercised somewhere is not a gap in the collection, however many
// other commits skipped it. The question is whether the accumulation holds
// any evidence for it, not whether every commit produced some.
func TestATaskExercisedOnOneCommitIsNotNeverExercised(t *testing.T) {
	first, second := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile, writeCorrespondence(t,
		`{"`+first+`":"build","`+second+`":"lint"}`))
	// Each commit exercises one of the two, so each skips the other and
	// neither is a gap. shadowEvidenceCommit always names the "build"
	// job, so the second commit is written out with its own.
	secondCommit := `{"plane":[{"task":"` + second + `","head_sha":"` + evidenceHeadTwo +
		`","observed":"success"}],` +
		`"actions":[{"job":"lint","head_sha":"` + evidenceHeadTwo + `","conclusion":"success"}]}`
	input := shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(first, evidenceHeadOne, "success", "success"),
		secondCommit)

	var document bytes.Buffer
	if err := githubShadowEvidence(strings.NewReader(input), &document); err != nil {
		t.Fatalf("githubShadowEvidence: %v", err)
	}
	report := decodeShadowEvidence(t, document.String())
	never, _ := report["never_exercised"].([]any)
	if len(never) != 0 {
		t.Fatalf("never_exercised %v, want empty", report["never_exercised"])
	}
	if strings.Contains(document.String(), "null") {
		t.Fatalf("report carries a null field:\n%s", document.String())
	}

	var page bytes.Buffer
	if err := githubEvidencePage(strings.NewReader(input), &page); err != nil {
		t.Fatalf("githubEvidencePage: %v", err)
	}
	// The line is omitted rather than written empty: a summary that says
	// nothing on every clean run teaches a reader to skip it.
	if strings.Contains(page.String(), "never exercised on any commit") {
		t.Fatalf("the summary is written with nothing to say:\n%s", page.String())
	}
	// The per-commit lines still carry what each one skipped.
	if !strings.Contains(page.String(), "not exercised on "+evidenceHeadOne+": "+second) {
		t.Fatalf("the per-commit coverage is gone:\n%s", page.String())
	}
}

// The document says which configuration produced the answer; the page is read
// while deciding whether a task may gate, and a digest is not part of that
// decision.
func TestTheEvidencePageLeavesTheDigestToTheDocument(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	document := shadowEvidenceInputDocument(1,
		shadowEvidenceCommit(task, evidenceHeadOne, "success", "success"))
	var asDocument, asPage bytes.Buffer
	if err := githubShadowEvidence(strings.NewReader(document), &asDocument); err != nil {
		t.Fatalf("settled evidence returned %v", err)
	}
	if err := githubEvidencePage(strings.NewReader(document), &asPage); err != nil {
		t.Fatalf("settled evidence page returned %v", err)
	}
	digest, _ := decodeShadowEvidence(t, asDocument.String())["catalog_digest"].(string)
	if digest == "" {
		t.Fatal("the document carries no catalog digest")
	}
	if strings.Contains(asPage.String(), digest) {
		t.Fatalf("the digest is on the page:\n%s", asPage.String())
	}
}

// A document the reader refuses is refused the same way by both writers, and
// neither writes anything first.
func TestTheEvidencePageRefusesWhatTheDocumentRefuses(t *testing.T) {
	shadowCompareEnv(t, "build")
	for _, testCase := range []struct {
		name  string
		input string
	}{
		{"no threshold", `{"commits":[]}`},
		{"no commits", `{"threshold":1,"commits":[]}`},
		{"trailing content", `{"threshold":1,"commits":[]} {}`},
		{"unknown field", `{"threshold":1,"commits":[],"required":["build"]}`},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			var out bytes.Buffer
			if err := githubEvidencePage(strings.NewReader(testCase.input), &out); err == nil {
				t.Fatal("an unreadable document was accepted")
			}
			if out.Len() != 0 {
				t.Fatalf("a refused document wrote a page anyway:\n%s", out.String())
			}
		})
	}
}

// A covered task this commit did not exercise that Actions did conclude is
// named on its own line. "Not exercised" reads as nothing to see here, and the
// other side having failed the job is the news.
func TestShadowCompareNamesATaskActionsJudgedWithoutARun(t *testing.T) {
	task, other := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile,
		writeCorrespondence(t, `{"`+task+`":"build","`+other+`":"test"}`))

	input := `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"},` +
		`{"job":"test","head_sha":"` + shadowCompareHead + `","conclusion":"failure"}]}`
	var out bytes.Buffer
	if err := githubShadowCompare(strings.NewReader(input), &out); err != nil {
		t.Fatalf("comparison refused: %v", err)
	}
	page := out.String()
	if !strings.Contains(page, "judged by Actions without a run on this side: "+other+" (test: failure)") {
		t.Fatalf("judged task not named: %q", page)
	}
	// Both facts are true and both are said: it was not exercised here,
	// and the other side judged it anyway.
	if !strings.Contains(page, "not exercised on this commit: "+other) {
		t.Fatalf("judged task dropped from the unexercised line: %q", page)
	}
}

// The judgement is reported, never graded. A verdict compared against nothing
// observed would manufacture agreement or conflict out of an absence, so a
// failing job for a task this side did not run leaves the comparison clean and
// the task out of the pairings.
func TestATaskJudgedWithoutARunIsNeverAPairing(t *testing.T) {
	task, other := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile,
		writeCorrespondence(t, `{"`+task+`":"build","`+other+`":"test"}`))

	input := `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"},` +
		`{"job":"test","head_sha":"` + shadowCompareHead + `","conclusion":"failure"}]}`
	var out bytes.Buffer
	if err := githubShadowCompare(strings.NewReader(input), &out); err != nil {
		t.Fatalf("a judgement without a run moved the verdict: %v", err)
	}
	if count := strings.Count(out.String(), other); count != 2 {
		t.Fatalf("judged task named %d times, want the unexercised line and the judged line: %q",
			count, out.String())
	}
}

// An unexercised task the Actions side stated no outcome for is not named as
// judged. An unfinished job and a job whose outcome was lost are not news.
func TestAnUnexercisedTaskActionsSaidNothingAboutIsNotNamedAsJudged(t *testing.T) {
	task, other := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile,
		writeCorrespondence(t, `{"`+task+`":"build","`+other+`":"test"}`))

	cases := map[string]string{
		"no job reported": ``,
		"job unfinished":  `,{"job":"test","head_sha":"` + shadowCompareHead + `","conclusion":""}`,
		"outcome lost":    `,{"job":"test","head_sha":"` + shadowCompareHead + `","conclusion":"stale"}`,
	}
	for name, extra := range cases {
		t.Run(name, func(t *testing.T) {
			input := `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
				`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"}` + extra + `]}`
			var out bytes.Buffer
			if err := githubShadowCompare(strings.NewReader(input), &out); err != nil {
				t.Fatalf("comparison refused: %v", err)
			}
			page := out.String()
			if strings.Contains(page, "judged by Actions") {
				t.Fatalf("a task nothing was stated about was named as judged: %q", page)
			}
			if !strings.Contains(page, "not exercised on this commit: "+other) {
				t.Fatalf("unexercised task not named: %q", page)
			}
		})
	}
}

// The Actions conclusion is given as Actions stated it. A skipped job and a
// failed one are different news, and collapsing them into "judged" would leave
// the one worth acting on indistinguishable from the one that is routine.
func TestThePageGivesTheConclusionForAnUnrunTaskVerbatim(t *testing.T) {
	task, other := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile,
		writeCorrespondence(t, `{"`+task+`":"build","`+other+`":"test"}`))

	for _, conclusion := range []string{"success", "failure", "cancelled", "skipped", "timed_out", "neutral", "action_required"} {
		t.Run(conclusion, func(t *testing.T) {
			input := `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
				`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"},` +
				`{"job":"test","head_sha":"` + shadowCompareHead + `","conclusion":"` + conclusion + `"}]}`
			var out bytes.Buffer
			if err := githubShadowCompare(strings.NewReader(input), &out); err != nil {
				t.Fatalf("comparison refused: %v", err)
			}
			want := "judged by Actions without a run on this side: " + other + " (test: " + conclusion + ")"
			if !strings.Contains(out.String(), want) {
				t.Fatalf("conclusion %q not given verbatim: %q", conclusion, out.String())
			}
		})
	}
}

// An ordinary commit says nothing about judgements without a run. A line that
// appears on every clean page teaches a reader to skip it.
func TestAnOrdinaryComparisonNamesNoJudgementWithoutARun(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	input := `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"}]}`
	var out bytes.Buffer
	if err := githubShadowCompare(strings.NewReader(input), &out); err != nil {
		t.Fatalf("comparison refused: %v", err)
	}
	if strings.Contains(out.String(), "judged by Actions") {
		t.Fatalf("clean page carries the judged line: %q", out.String())
	}
}
