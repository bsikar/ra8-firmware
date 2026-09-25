// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"crypto/tls"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/agent"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/asciigate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardsweep"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/committerms"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/neutral"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/newlinegate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/runnerclock"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/scaler"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/server"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/sincegate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testsreadme"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/migrations"
	"github.com/jackc/pgx/v5/pgxpool"
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	code := run(ctx, os.Args[1:])
	stop()
	os.Exit(code)
}

func run(ctx context.Context, args []string) int {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "usage: ra8ci <task>|tasks [--digest|--json]|ascii [--check] [--all|PATH]|since [--all|FILE...]|final-newline [FILE...]|runner-clock [--repo OWNER/REPO] [--runs N] [--hours N]|tests-readme [--selftest]|inclusive-terminology-commits [--selftest]|server|agent|sync|backup refresh|keygen|board status|take [--class human|ci|agent]|checkpoint|extend|cancel|hil budget|verify-capture|db migrate|report slow|github check|github shadow|github shadow-compare|github publish-check-run|github required-checks|github evidence-gate|github gate|github actions-run|github reconcile|github pull-request|github evidence-run|run submit|run status")
		return 2
	}
	var err error
	switch args[0] {
	case "tasks":
		var cat *catalog.Catalog
		cat, err = catalog.Load()
		if err == nil {
			err = tasksCommand(os.Stdout, cat, args[1:])
			if errors.Is(err, errTasksUsage) {
				return usageError(err.Error())
			}
		}
	case "server":
		if len(args) != 1 {
			return usageError("server takes no arguments")
		}
		err = serve(ctx)
	case "agent":
		if len(args) != 1 {
			return usageError("agent takes no arguments")
		}
		err = runAgent(ctx)
	case "board-agent":
		if len(args) != 1 {
			return usageError("board-agent takes no arguments")
		}
		err = runBoardAgent(ctx)
	case "sync":
		if len(args) != 1 {
			return usageError("sync takes no arguments")
		}
		err = syncLocalRuns(ctx)
	case "github":
		err = githubCommand(ctx, args[1:])
	case "backup":
		err = backupCommand(ctx, args[1:])
	case "board":
		err = boardCommand(ctx, args[1:])
	case "hil":
		err = hilCommand(ctx, args[1:])
	case "ascii":
		if len(args) == 1 {
			return runLocalTask(ctx, []string{"ascii"})
		}
		var root string
		root, err = findCheckout()
		if err == nil {
			return asciigate.Run(ctx, root, args[1:], os.Stdout, os.Stderr)
		}
	case "since":
		if len(args) == 1 {
			return runLocalTask(ctx, []string{"since"})
		}
		var root string
		root, err = findCheckout()
		if err == nil {
			return sincegate.Run(ctx, root, args[1:], os.Stdout, os.Stderr)
		}
	case "final-newline":
		if len(args) == 1 {
			return runLocalTask(ctx, []string{"final-newline"})
		}
		var root string
		root, err = findCheckout()
		if err == nil {
			return newlinegate.Run(ctx, root, args[1:], os.Stdout, os.Stderr)
		}
	case "runner-clock":
		if len(args) == 1 {
			return runLocalTask(ctx, []string{"runner-clock"})
		}
		return runnerclock.Run(ctx, args[1:], os.Stdout, os.Stderr)
	case "tests-readme":
		if len(args) == 1 {
			return runLocalTask(ctx, []string{"tests-readme"})
		}
		var root string
		root, err = findCheckout()
		if err == nil {
			return testsreadme.Run(ctx, root, args[1:], os.Stdout, os.Stderr)
		}
	case "inclusive-terminology-commits":
		if len(args) == 1 {
			return runLocalTask(ctx, []string{"inclusive-terminology-commits"})
		}
		return committerms.Run(ctx, args[1:], os.Stdin, os.Stdout, os.Stderr)
	case "db":
		if len(args) != 2 || args[1] != "migrate" {
			return usageError("usage: ra8ci db migrate")
		}
		err = migrate(ctx)
	case "report":
		err = report(ctx, args[1:])
	case "run":
		err = runCommand(ctx, args[1:])
	default:
		return runLocalTask(ctx, args)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "ra8ci:", err)
		return 1
	}
	return 0
}

func runLocalTask(ctx context.Context, args []string) int {
	cat, err := catalog.Load()
	if err != nil {
		fmt.Fprintln(os.Stderr, "ra8ci:", err)
		return 1
	}
	task, ok := cat.Task(args[0])
	if !ok {
		return usageError("unknown task " + args[0])
	}
	values, err := taskArgumentValues(task, args[1:])
	if err != nil {
		return usageError(err.Error())
	}
	bound, err := task.BindArguments(values)
	if err != nil {
		return usageError(err.Error())
	}
	if !task.IsSafeLocal() {
		return usageError("task requires server dispatch")
	}
	root, err := findCheckout()
	if err != nil {
		fmt.Fprintln(os.Stderr, "ra8ci:", err)
		return 1
	}
	stateDirectory, err := spool.DefaultDirectory()
	if err != nil {
		fmt.Fprintln(os.Stderr, "ra8ci:", err)
		return 1
	}
	localSpool, err := spool.Open(stateDirectory)
	if err != nil {
		fmt.Fprintln(os.Stderr, "ra8ci:", err)
		return 1
	}
	metadata := spool.Metadata{Tier: task.Tier, Scope: task.Scope,
		DeadlineSeconds: task.DeadlineSeconds, Args: append([]string(nil), bound...)}
	metadata.Source, err = localSourceIdentity(ctx, root)
	var started spool.Entry
	if err != nil {
		fmt.Fprintln(os.Stderr, "ra8ci: source identity unavailable; local result will remain unverified:", err)
		started, err = localSpool.Begin(task.Name, cat.Digest())
	} else {
		started, err = localSpool.BeginWithMetadata(task.Name, cat.Digest(), metadata)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "ra8ci:", err)
		return 1
	}
	result, err := executor.RunWithArguments(ctx, root, task, values, os.Stdout, os.Stderr)
	finished, recordErr := localSpool.Finish(started, result, err)
	if recordErr != nil {
		fmt.Fprintln(os.Stderr, "ra8ci: unable to persist local result:", recordErr)
		return 1
	}
	fmt.Fprintln(os.Stderr, "ra8ci: local run", finished.ID, "unsynced")
	if err != nil {
		fmt.Fprintln(os.Stderr, "ra8ci:", err)
		return 1
	}
	if result.TimedOut {
		fmt.Fprintln(os.Stderr, "ra8ci: task deadline exceeded")
		return 124
	}
	if result.Cancelled {
		fmt.Fprintln(os.Stderr, "ra8ci: task cancelled")
		return 130
	}
	return result.ExitCode
}

func localSourceIdentity(ctx context.Context, root string) (spool.SourceIdentity, error) {
	repository := os.Getenv("RA8CI_REPOSITORY")
	if repository == "" {
		repository = "bsikar/ra8-firmware"
	}
	command := exec.CommandContext(ctx, "git", "-C", root, "rev-parse", "--verify", "HEAD")
	output, err := command.Output()
	if err != nil {
		return spool.SourceIdentity{}, fmt.Errorf("read local commit: %w", err)
	}
	identity := spool.SourceIdentity{Repository: repository,
		CommitSHA: strings.TrimSpace(string(output)), Verification: "unverified"}
	branchCommand := exec.CommandContext(ctx, "git", "-C", root, "symbolic-ref", "--short", "-q", "HEAD")
	if branch, branchErr := branchCommand.Output(); branchErr == nil {
		identity.Branch = strings.TrimSpace(string(branch))
	}
	snapshotContext, stop := context.WithTimeout(ctx, 30*time.Second)
	defer stop()
	if snapshot, snapshotErr := source.Snapshot(snapshotContext, root); snapshotErr == nil &&
		snapshot.RootCommit == identity.CommitSHA {
		identity.SnapshotSHA256 = snapshot.Digest
		identity.Verification = "verified"
	}
	return identity, nil
}

func findCheckout() (string, error) {
	directory, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for {
		if _, err := os.Stat(filepath.Join(directory, ".git")); err == nil {
			return catalog.VerifyCheckout(directory)
		}
		parent := filepath.Dir(directory)
		if parent == directory {
			return "", errors.New("no repository checkout found")
		}
		directory = parent
	}
}

func migrate(ctx context.Context) error {
	dsn := os.Getenv("RA8CI_MIGRATION_DATABASE_URL")
	if dsn == "" {
		return errors.New("RA8CI_MIGRATION_DATABASE_URL is required")
	}
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		return err
	}
	defer pool.Close()
	return migrations.Apply(ctx, pool)
}

func report(ctx context.Context, args []string) error {
	if len(args) == 0 || args[0] != "slow" {
		return errors.New("usage: ra8ci report slow [--window 168h] [--limit 25]")
	}
	flags := flag.NewFlagSet("report slow", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	windowText := flags.String("window", "168h", "report window")
	limit := flags.Int("limit", 25, "maximum rows")
	if err := flags.Parse(args[1:]); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("report slow accepts no positional arguments")
	}
	window, err := time.ParseDuration(*windowText)
	if err != nil || window <= 0 || window > 365*24*time.Hour {
		return errors.New("report window must be a positive duration no longer than one year")
	}
	if *limit < 1 || *limit > 500 {
		return errors.New("report limit must be between 1 and 500")
	}
	repository := os.Getenv("RA8CI_REPOSITORY")
	if repository == "" {
		repository = "bsikar/ra8-firmware"
	}
	rows, err := fetchSlowReport(ctx, repository, window, *limit)
	if err != nil {
		return err
	}

	return json.NewEncoder(os.Stdout).Encode(rows)
}

func serve(ctx context.Context) error {
	dsn := os.Getenv("RA8CI_DATABASE_URL")
	certPath := os.Getenv("RA8CI_TLS_CERT")
	keyPath := os.Getenv("RA8CI_TLS_KEY")
	caPath := os.Getenv("RA8CI_CLIENT_CA")
	listenAddress := os.Getenv("RA8CI_LISTEN_ADDR")
	stateKeyPath := os.Getenv("RA8CI_TERRAFORM_STATE_KEY_FILE")
	boardAgentKeyPath := os.Getenv("RA8CI_BOARD_AGENT_KEYS_FILE")
	if dsn == "" || certPath == "" || keyPath == "" || caPath == "" || listenAddress == "" || stateKeyPath == "" {
		return errors.New("server requires RA8CI_DATABASE_URL, RA8CI_TLS_CERT, RA8CI_TLS_KEY, RA8CI_CLIENT_CA, RA8CI_LISTEN_ADDR, and RA8CI_TERRAFORM_STATE_KEY_FILE")
	}
	boardVerifier, err := neutral.LoadVerifierFile(boardAgentKeyPath)
	if err != nil {
		return fmt.Errorf("load board-agent trust allowlist: %w", err)
	}
	backupPublicKeyPath := os.Getenv("RA8CI_BACKUP_PUBLIC_KEY_FILE")
	backupAttestationPath := os.Getenv("RA8CI_BACKUP_ATTESTATION_FILE")
	backupApprovalID := os.Getenv("RA8CI_BACKUP_APPROVAL_ID")
	if backupPublicKeyPath == "" || backupAttestationPath == "" || backupApprovalID == "" {
		return errors.New("server requires signed backup readiness configuration")
	}
	backupPublicKey, err := scaler.LoadBackupPublicKey(backupPublicKeyPath)
	if err != nil {
		return err
	}
	backupGate, err := scaler.NewSignedBackupGate(backupAttestationPath, backupPublicKey, backupApprovalID, 20*time.Minute, 48*time.Hour, 90*24*time.Hour)
	if err != nil {
		return err
	}
	if err := backupGate.Check(ctx, backupApprovalID); err != nil {
		return fmt.Errorf("server startup blocked by off-VM backup/restore readiness: %w", err)
	}
	// The listener's own identity and the authorities it trusts clients from
	// are both decided here, before the socket opens. Loading them without
	// deciding anything moves every refusal to the first handshake, where an
	// operator sees a connection reset and cannot tell a stale certificate
	// apart from a missing grant.
	cert, err := mtls.LoadServerIdentity(certPath, keyPath, time.Now())
	if err != nil {
		return fmt.Errorf("load TLS identity: %w", err)
	}
	caPEM, err := os.ReadFile(caPath)
	if err != nil {
		return fmt.Errorf("load client CA: %w", err)
	}
	clientCAs, err := mtls.ClientAuthorities(caPEM, time.Now())
	if err != nil {
		return fmt.Errorf("load client CA: %w", err)
	}
	stateKeyInfo, err := os.Lstat(stateKeyPath)
	if err != nil {
		return fmt.Errorf("stat Terraform state key: %w", err)
	}
	if !stateKeyInfo.Mode().IsRegular() || stateKeyInfo.Mode().Perm()&0o077 != 0 {
		return errors.New("Terraform state key must be a regular file readable only by its owner")
	}
	stateKey, err := os.ReadFile(stateKeyPath)
	if err != nil {
		return fmt.Errorf("read Terraform state key: %w", err)
	}
	decodedStateKey, err := base64.StdEncoding.Strict().DecodeString(string(stateKey))
	clear(stateKey)
	if err != nil {
		return fmt.Errorf("decode Terraform state key: %w", err)
	}
	st, err := store.OpenWithTerraformStateKey(ctx, dsn, decodedStateKey)
	clear(decodedStateKey)
	if err != nil {
		return err
	}
	defer st.Close()
	cat, err := catalog.Load()
	if err != nil {
		return err
	}
	api, err := server.NewWithOptions(st, cat, boardVerifier, os.Getenv("RA8CI_AGENT_TRUSTED_COMMIT"),
		func(checkCtx context.Context) error { return backupGate.Check(checkCtx, backupApprovalID) })
	if err != nil {
		return err
	}
	tlsConfig := &tls.Config{MinVersion: tls.VersionTLS13, ClientAuth: tls.RequireAndVerifyClientCert, ClientCAs: clientCAs, Certificates: []tls.Certificate{cert}}
	listener, err := net.Listen("tcp", listenAddress)
	if err != nil {
		return err
	}
	defer listener.Close()
	httpServer := &http.Server{Handler: api.Handler(), TLSConfig: tlsConfig, ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 30 * time.Second, WriteTimeout: 30 * time.Second, IdleTimeout: 60 * time.Second}
	serverCtx, serverCancel := context.WithCancel(ctx)
	defer serverCancel()
	done := make(chan struct{})
	go func() {
		defer close(done)
		<-serverCtx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = httpServer.Shutdown(shutdownCtx)
	}()
	// A board whose holder died is only reclaimed when something asks it a
	// question, and an idle bench is asked nothing. This is what gives an
	// idle board a clock; it runs on the maintenance tick already here
	// rather than a timer of its own, so a sweep and the server can never
	// disagree about whether this process is still serving.
	boardSweeper, err := boardsweep.New(st, 100)
	if err != nil {
		return err
	}
	maintenanceFailed := make(chan error, 1)
	go func() {
		ticker := time.NewTicker(15 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-serverCtx.Done():
				return
			case <-ticker.C:
				checkCtx, stop := context.WithTimeout(serverCtx, 10*time.Second)
				_, maintenanceErr := st.ReapAgentAssignments(checkCtx, cat, 100)
				if maintenanceErr == nil {
					var swept boardsweep.Report
					swept, maintenanceErr = boardSweeper.Pass(checkCtx, time.Now().UTC())
					// The counts are what happened either way, so
					// they are reported before the error is, and a
					// quiet bench prints nothing.
					if swept.Notable() {
						fmt.Fprintln(os.Stderr, "ra8ci:", swept)
					}
					if maintenanceErr != nil {
						maintenanceErr = fmt.Errorf("reclaim expired board leases: %w", maintenanceErr)
					}
				} else {
					maintenanceErr = fmt.Errorf("reap expired agent assignments: %w", maintenanceErr)
				}
				stop()
				if maintenanceErr != nil && serverCtx.Err() == nil {
					maintenanceFailed <- maintenanceErr
					serverCancel()
					return
				}
			}
		}
	}()
	err = httpServer.Serve(tls.NewListener(listener, tlsConfig))
	serverCancel()
	<-done
	select {
	case maintenanceErr := <-maintenanceFailed:
		return maintenanceErr
	default:
	}
	if !errors.Is(err, http.ErrServerClosed) {
		return err
	}
	return nil
}

func runAgent(ctx context.Context) error {
	endpoint, err := resolveClientEndpoint("ra8ci agent", roleRunnerAgent, os.Getenv, envAgentRoot)
	if err != nil {
		return err
	}
	config := agent.Config{
		ServerURL: endpoint.ServerURL,
		CAFile:    endpoint.CAFile,
		CertFile:  endpoint.CertFile,
		KeyFile:   endpoint.KeyFile,
		Root:      os.Getenv(envAgentRoot),
	}
	if text := os.Getenv("RA8CI_AGENT_POLL_WAIT"); text != "" {
		wait, err := time.ParseDuration(text)
		if err != nil || wait < 0 || wait > 25*time.Second {
			return errors.New("RA8CI_AGENT_POLL_WAIT must be a duration between zero and 25 seconds")
		}
		config.PollWait = wait
	}
	client, err := agent.New(config)
	if err != nil {
		return err
	}
	err = client.Run(ctx)
	if errors.Is(err, context.Canceled) {
		return nil
	}
	return err
}

// githubCommand dispatches the GitHub subcommands. check, shadow,
// shadow-compare and required-checks change nothing on GitHub, and gate only
// reads it; publish-check-run is the one that writes, and it says so in its own
// documentation.
func githubCommand(ctx context.Context, args []string) error {
	if len(args) != 1 {
		return errors.New("usage: ra8ci github check|shadow|shadow-compare|shadow-evidence|publish-check-run|required-checks|evidence-gate|gate|actions-run|reconcile|pull-request|evidence-run|pull-request-evidence")
	}
	switch args[0] {
	case "check":
		return githubSessionCheck(ctx)
	case "shadow":
		return githubShadowConfig(os.Stdout)
	case "shadow-compare":
		return githubShadowCompare(os.Stdin, os.Stdout)
	case "publish-check-run":
		return githubPublishCheckRuns(ctx, os.Stdin, os.Stdout)
	case "shadow-evidence":
		return githubShadowEvidence(os.Stdin, os.Stdout)
	case "required-checks":
		return githubRequiredChecks(os.Stdin, os.Stdout)
	case "evidence-gate":
		return githubEvidenceGate(os.Stdin, os.Stdout)
	case "gate":
		return githubGate(ctx, os.Stdin, os.Stdout)
	case "actions-run":
		return githubActionsRun(ctx, os.Stdin, os.Stdout)
	case "reconcile":
		return githubReconcileCheckRuns(ctx, os.Stdin, os.Stdout)
	case "pull-request":
		return githubPullRequestRuns(ctx, os.Stdin, os.Stdout)
	case "evidence-run":
		return githubEvidenceRun(ctx, os.Stdin, os.Stdout)
	case "pull-request-evidence":
		return githubPullRequestEvidence(ctx, os.Stdin, os.Stdout)
	default:
		return errors.New("usage: ra8ci github check|shadow|shadow-compare|shadow-evidence|publish-check-run|required-checks|evidence-gate|gate|actions-run|reconcile|pull-request|evidence-run|pull-request-evidence")
	}
}

// maxShadowComparisonBytes bounds the observation document this reads. One
// commit's outcomes across the whole catalog are a few kilobytes, so this is
// room to spare and still a refusal rather than an unbounded read of whatever
// is piped in.
const maxShadowComparisonBytes = 256 << 10

// maxShadowEvidenceBytes bounds the evidence document. It holds one
// comparison document per pull request rather than one commit's, so it is
// larger than maxShadowComparisonBytes, and still bounded: the readiness
// answer is read by a person, and a document nobody could review is not
// evidence anybody is weighing.
const maxShadowEvidenceBytes = 4 << 20

// shadowComparisonInput is the wire shape this command reads. The field names
// are declared here rather than as tags on PlaneOutcome and ActionsOutcome,
// because a wire contract on those types would outlive this command and they
// were written as in-process values.
type shadowComparisonInput struct {
	// ActionsRun names the workflow run the Actions half came from. It is
	// optional, because a document assembled by hand has no run to name,
	// and checked when present: evidence that does not say which attempt
	// it graded cannot be checked against the run a second reader sees.
	ActionsRun *actionsRunAnchor     `json:"actions_run,omitempty"`
	Plane      []planeOutcomeInput   `json:"plane,omitempty"`
	Actions    []actionsOutcomeInput `json:"actions,omitempty"`
}

// planeOutcomeInput is one task's terminal conclusion as this plane observed
// it.
type planeOutcomeInput struct {
	Task     string `json:"task"`
	HeadSHA  string `json:"head_sha"`
	Observed string `json:"observed"`
}

// actionsOutcomeInput is one workflow job's conclusion.
type actionsOutcomeInput struct {
	Job        string `json:"job"`
	HeadSHA    string `json:"head_sha"`
	Conclusion string `json:"conclusion"`
}

// actionsRunAnchor says which workflow run and attempt the Actions half was
// read from.
type actionsRunAnchor struct {
	RunID   int64  `json:"run_id"`
	Attempt int    `json:"attempt"`
	HeadSHA string `json:"head_sha"`
}

// shadowEvidenceInput is several pull requests' comparisons and the number of
// graded commits the reader is asking for.
type shadowEvidenceInput struct {
	Threshold int                     `json:"threshold"`
	Commits   []shadowComparisonInput `json:"commits"`
}

// githubShadowCompare grades one commit's shadow run against Actions and
// renders the page the required-check decision is read from.
//
// The two sides are read from a document rather than fetched, for the same
// reason the correspondence is the caller's statement: which job covers which
// task is a claim somebody makes, and a command that went and collected both
// sides itself would be making that claim silently.
func githubShadowCompare(in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}

	var document shadowComparisonInput
	decoder := json.NewDecoder(io.LimitReader(in, maxShadowComparisonBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read shadow observations: %w", err)
	}
	// Anything after the object is a second document, which means the
	// input is not the one commit it is read as.
	if decoder.More() {
		return errors.New("read shadow observations: trailing content after the document")
	}
	if decoder.InputOffset() > maxShadowComparisonBytes {
		return fmt.Errorf("read shadow observations: larger than %d bytes", maxShadowComparisonBytes)
	}

	plane := make([]github.PlaneOutcome, 0, len(document.Plane))
	for _, outcome := range document.Plane {
		plane = append(plane, github.PlaneOutcome{
			Task: outcome.Task, HeadSHA: outcome.HeadSHA, Observed: outcome.Observed,
		})
	}
	actions := make([]github.ActionsOutcome, 0, len(document.Actions))
	for _, outcome := range document.Actions {
		actions = append(actions, github.ActionsOutcome{
			Job: outcome.Job, HeadSHA: outcome.HeadSHA, Conclusion: outcome.Conclusion,
		})
	}

	collection, err := config.Correspondence.Collect(plane, actions)
	if err != nil {
		return fmt.Errorf("collect shadow observations: %w", err)
	}
	// An anchor naming another commit is refused rather than ignored. It is
	// how one run's outcomes end up filed under another run's attempt, and
	// an attempt that does not describe the evidence under it is worse than
	// no attempt at all.
	if document.ActionsRun != nil && !strings.EqualFold(document.ActionsRun.HeadSHA, collection.HeadSHA) {
		return fmt.Errorf("shadow observations are about %s, the run they name is about %s",
			collection.HeadSHA, document.ActionsRun.HeadSHA)
	}
	report, err := github.CompareShadowRun(collection.Observations)
	if err != nil {
		return fmt.Errorf("compare shadow run: %w", err)
	}
	if err := github.RenderShadowReport(out, report); err != nil {
		return fmt.Errorf("render shadow comparison: %w", err)
	}
	// The tasks this commit did not exercise are named after the page
	// rather than folded into it. A task selection that skipped them is a
	// normal commit, not a gap in the evidence, so they are not pairings
	// and must not read as any.
	if len(collection.NotRun) > 0 {
		if _, err := fmt.Fprintf(out, "\nnot exercised on this commit: %s\n",
			strings.Join(collection.NotRun, ", ")); err != nil {
			return fmt.Errorf("render shadow comparison: %w", err)
		}
	}
	// The verdict is the exit status, and the page is written first. A
	// caller that reads only the status must not be able to get a clean
	// one from a report nobody could read.
	if !report.Clean() {
		return fmt.Errorf("shadow comparison is not clean: %d conflicting, %d indeterminate",
			report.Conflicting, report.Indeterminate)
	}
	return nil
}

// maxCheckRunPublishBytes bounds the outcome document this reads. One commit's
// outcomes across the whole catalog are a few kilobytes.
const maxCheckRunPublishBytes = 256 << 10

// checkRunPublishInput is the wire shape publish-check-run reads: one commit,
// and what the plane observed for each task it ran.
type checkRunPublishInput struct {
	HeadSHA string `json:"head_sha"`
	Runs    []struct {
		Task    string `json:"task"`
		State   string `json:"state"`
		Summary string `json:"summary"`
	} `json:"runs"`
}

// plannedCheckRun is one check run and the body it will carry, built and
// checked before anything is posted.
type plannedCheckRun struct {
	Task    string
	Run     github.TaskCheckRun
	Summary string
}

// planCheckRuns turns one commit's observed outcomes into the runs to post.
//
// Every run is built before any is posted. A check run cannot be taken back
// once GitHub has it, so a document with a bad task in the middle must be
// refused whole rather than half published.
func planCheckRuns(mode github.CheckRunMode, correspondence *github.ShadowCorrespondence, document checkRunPublishInput) ([]plannedCheckRun, error) {
	if len(document.Runs) == 0 {
		return nil, errors.New("no task outcomes to publish")
	}
	planned := make([]plannedCheckRun, 0, len(document.Runs))
	seen := make(map[string]bool, len(document.Runs))
	for _, outcome := range document.Runs {
		// Two runs for one task on one commit post two check runs under
		// the same name, and which one a gate reads is then a race.
		if seen[outcome.Task] {
			return nil, fmt.Errorf("task %q appears twice for this commit", outcome.Task)
		}
		seen[outcome.Task] = true
		// The correspondence is checked against the catalog when it is
		// loaded, so covering a task is also the statement that the
		// task exists. An uncovered task is refused rather than
		// dropped: its shadow run could be posted but never graded,
		// which is a run published as evidence that nothing reads.
		if _, covered := correspondence.Job(outcome.Task); !covered {
			return nil, fmt.Errorf("task %q is not covered by the declared correspondence", outcome.Task)
		}
		run, err := github.NewTaskCheckRun(mode, outcome.Task, document.HeadSHA, outcome.State)
		if err != nil {
			return nil, err
		}
		summary := strings.TrimSpace(outcome.Summary)
		if summary == "" {
			// A blank summary publishes a check run a reviewer
			// cannot act on. The composed one restates facts the
			// run already carries rather than inventing any.
			summary = fmt.Sprintf("ra8ci observed %s for task %s on %s.", run.Observed, outcome.Task, run.HeadSHA)
		}
		planned = append(planned, plannedCheckRun{Task: outcome.Task, Run: run, Summary: summary})
	}
	return planned, nil
}

// reconciledCheckRun is one planned run and what the commit's already
// published runs mean for it.
type reconciledCheckRun struct {
	Plan    plannedCheckRun
	Verdict github.ReconciledPublish
}

// reconcileCheckRunPlan decides the whole plan against one listing of what is
// already on the commit, before anything is posted.
//
// This is the implementation contract's rule at the command: an uncertain
// Checks API write is reconciled by listing the commit's runs, never blindly
// repeated. A second post is not a correction; GitHub keeps each post as its
// own run, so repeating one leaves two runs under a name branch protection
// may one day require, and which of them a gate reads is then a race.
//
// A conflict refuses the document whole rather than skipping the one task.
// The other runs in the document are about the same commit and the same
// workflow, and a commit already carrying a run that says something else is
// evidence about this deployment, not about that one task; publishing the
// rest would bury the disagreement under fresh runs while an operator is
// still working out where it came from.
func reconcileCheckRunPlan(planned []plannedCheckRun, published github.PublishedCheckRuns) ([]reconciledCheckRun, error) {
	if len(planned) == 0 {
		return nil, errors.New("no check runs to reconcile")
	}
	reconciled := make([]reconciledCheckRun, 0, len(planned))
	for _, plan := range planned {
		verdict, err := github.ReconcilePublish(plan.Run, published)
		if err != nil {
			return nil, fmt.Errorf("reconcile %s: %w", plan.Task, err)
		}
		if verdict.Decision == github.PublishConflicts {
			// The two conflicts send an operator to different places.
			// A run this plane did not publish is a question about who
			// holds a checks:write token on the repository, and saying
			// only that the commit disagrees would have somebody go
			// looking through this deployment's own history for a run
			// that was never in it.
			if len(verdict.Unclaimed) > 0 {
				return nil, fmt.Errorf("task %s already has a check run on %s this plane did not publish: %s",
					plan.Task, plan.Run.HeadSHA, describePublishedRuns(verdict.Unclaimed))
			}
			return nil, fmt.Errorf("task %s already has a check run on %s saying something else: %s",
				plan.Task, plan.Run.HeadSHA, describePublishedRuns(verdict.Existing))
		}
		reconciled = append(reconciled, reconciledCheckRun{Plan: plan, Verdict: verdict})
	}
	return reconciled, nil
}

// describePublishedRuns names the runs a decision was made from, so a refusal
// points at the runs an operator has to open rather than at the name alone.
func describePublishedRuns(runs []github.PublishedCheckRun) string {
	described := make([]string, 0, len(runs))
	for _, run := range runs {
		described = append(described, fmt.Sprintf("%s #%d %s", run.Name, run.ID, publishedState(run)))
	}
	return strings.Join(described, ", ")
}

// publishedState is a run's conclusion, or its status while it has none.
func publishedState(run github.PublishedCheckRun) string {
	if run.Conclusion == "" {
		return run.Status
	}
	return run.Conclusion
}

// decisionToken renders a decision as one word for the report, taken from the
// decision's own name so the two cannot drift apart.
func decisionToken(decision github.PublishDecision) string {
	return strings.ReplaceAll(decision.String(), " ", "-")
}

// githubPublishCheckRuns posts one check run per task outcome for one commit.
//
// This is the first ra8ci command that writes to GitHub. What it may write is
// configuration, not an argument: the mode comes from the environment, so a
// shadow deployment posts runs that report neutral and cannot hold a pull
// request, and moving onto the merge gate is a deployment change.
//
// Nothing is posted before the commit's existing check runs are read. A run
// this plane already published under the same name is left alone, a write
// still in flight is waited for rather than repeated, and a run that
// disagrees stops the whole document for an operator to settle.
func githubPublishCheckRuns(ctx context.Context, in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}
	publisherConfig, enabled, err := github.LoadCheckRunPublisherConfigFromEnv(config.Mode)
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing has no repository: set %s",
			github.EnvCheckRunRepository)
	}

	var document checkRunPublishInput
	decoder := json.NewDecoder(io.LimitReader(in, maxCheckRunPublishBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read task outcomes: %w", err)
	}
	if decoder.More() {
		return errors.New("read task outcomes: trailing content after the document")
	}
	if decoder.InputOffset() > maxCheckRunPublishBytes {
		return fmt.Errorf("read task outcomes: larger than %d bytes", maxCheckRunPublishBytes)
	}

	planned, err := planCheckRuns(config.Mode, config.Correspondence, document)
	if err != nil {
		return fmt.Errorf("plan check runs: %w", err)
	}

	publisher, err := github.NewCheckRunPublisher(publisherConfig)
	if err != nil {
		return err
	}
	// The reconciler reads with its own narrow token. It asks for
	// checks:read where the publisher holds checks:write, so the read
	// that decides whether to write cannot itself write, and an
	// installation granting write can still mint it.
	reconciler, err := github.NewCheckRunReconciler(github.CheckRunReconcilerConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}
	// One listing decides the whole document. Reading the commit once per
	// task would let the answer change underneath the plan, and every run
	// here is about the same commit.
	commit := planned[0].Run.HeadSHA
	published, err := reconciler.PublishedRuns(ctx, commit)
	if err != nil {
		return fmt.Errorf("read the check runs already on %s: %w", commit, err)
	}
	reconciled, err := reconcileCheckRunPlan(planned, published)
	if err != nil {
		return err
	}

	posted := 0
	needed := 0
	for _, decided := range reconciled {
		if decided.Verdict.Repeat() {
			needed++
		}
	}
	waiting := make([]string, 0, len(reconciled))
	for _, decided := range reconciled {
		if !decided.Verdict.Repeat() {
			// A run this plane has already published is reported
			// with the run that settles it, so an operator reading
			// the output can open it rather than take the word for
			// it.
			for _, existing := range decided.Verdict.Existing {
				if _, err := fmt.Fprintf(out, "%s %d %s %s\n", decisionToken(decided.Verdict.Decision),
					existing.ID, publishedState(existing), existing.Name); err != nil {
					return fmt.Errorf("report published check run: %w", err)
				}
			}
			if decided.Verdict.Decision == github.PublishInFlight {
				waiting = append(waiting, decided.Plan.Task)
			}
			continue
		}
		id, err := publisher.Publish(ctx, decided.Plan.Run, decided.Plan.Summary)
		if err != nil {
			// A partial publish is not a failed publish. The runs
			// already posted are on the commit whatever this
			// command returns, so the count is reported rather
			// than left for the operator to guess.
			return fmt.Errorf("publish %s after %d of %d posted: %w", decided.Plan.Task, posted, needed, err)
		}
		posted++
		if _, err := fmt.Fprintf(out, "%s %d %s %s\n", decisionToken(github.PublishNeeded),
			id, decided.Plan.Run.Conclusion, decided.Plan.Run.Name); err != nil {
			return fmt.Errorf("report published check run: %w", err)
		}
	}
	// A run left to a write already in flight is not an error in the
	// document, it is the answer: the post landed and its answer did not,
	// and the caller has to read the commit again rather than treat it as
	// published. The report is written first, so an exit status is never
	// the only thing that carries it.
	if len(waiting) > 0 {
		return fmt.Errorf("%d of %d check runs were left to a write already in flight on %s: %s",
			len(waiting), len(reconciled), commit, strings.Join(waiting, ", "))
	}
	return nil
}

// reconcileSurvey is one task's standing on the commit, as the read-only
// survey reports it.
type reconcileSurvey struct {
	Task      string                 `json:"task"`
	Name      string                 `json:"name"`
	Decision  string                 `json:"decision"`
	Published []reconcileSurveyedRun `json:"published"`
}

// reconcileSurveyedRun is one run already on the commit under that task's
// name. Ours reports whether this plane published it, which is the whole of
// why a run that reads identically can still be a conflict.
type reconcileSurveyedRun struct {
	ID         int64  `json:"id"`
	Status     string `json:"status"`
	Conclusion string `json:"conclusion"`
	ExternalID string `json:"external_id"`
	Ours       bool   `json:"ours"`
}

// reconcileReport is the whole survey of one commit.
type reconcileReport struct {
	Commit   string            `json:"commit"`
	Mode     string            `json:"mode"`
	Settled  bool              `json:"settled"`
	Posting  int               `json:"posting"`
	Waiting  int               `json:"waiting"`
	Conflict int               `json:"conflicting"`
	Tasks    []reconcileSurvey `json:"tasks"`
}

// surveyCheckRunPlan decides every planned run against one listing and reports
// all of them.
//
// It is deliberately NOT reconcileCheckRunPlan. That function refuses the
// whole document on a conflict, because it is about to post the rest and
// publishing over a disagreement buries it. Nothing is posted here, so
// refusing would only withhold the picture an operator came for: the
// conflicting task is reported beside the others, and the exit status carries
// the fact that one was found.
func surveyCheckRunPlan(planned []plannedCheckRun, published github.PublishedCheckRuns) (reconcileReport, error) {
	if len(planned) == 0 {
		return reconcileReport{}, errors.New("no check runs to survey")
	}
	report := reconcileReport{
		Commit: planned[0].Run.HeadSHA,
		Mode:   planned[0].Run.Mode.String(),
		Tasks:  make([]reconcileSurvey, 0, len(planned)),
	}
	for _, plan := range planned {
		verdict, err := github.ReconcilePublish(plan.Run, published)
		if err != nil {
			return reconcileReport{}, fmt.Errorf("reconcile %s: %w", plan.Task, err)
		}
		switch verdict.Decision {
		case github.PublishNeeded:
			report.Posting++
		case github.PublishInFlight:
			report.Waiting++
		case github.PublishConflicts:
			report.Conflict++
		}
		unclaimed := make(map[int64]bool, len(verdict.Unclaimed))
		for _, run := range verdict.Unclaimed {
			unclaimed[run.ID] = true
		}
		surveyed := make([]reconcileSurveyedRun, 0, len(verdict.Existing))
		for _, run := range verdict.Existing {
			surveyed = append(surveyed, reconcileSurveyedRun{
				ID:         run.ID,
				Status:     run.Status,
				Conclusion: run.Conclusion,
				ExternalID: run.ExternalID,
				Ours:       !unclaimed[run.ID],
			})
		}
		report.Tasks = append(report.Tasks, reconcileSurvey{
			Task:      plan.Task,
			Name:      plan.Run.Name,
			Decision:  decisionToken(verdict.Decision),
			Published: surveyed,
		})
	}
	report.Settled = report.Posting == 0 && report.Waiting == 0 && report.Conflict == 0
	return report, nil
}

// githubReconcileCheckRuns reports what one commit already carries for a
// document of task outcomes, and writes nothing to GitHub.
//
// publish-check-run already reads the commit before it posts, and when it
// leaves a task to a write still in flight it tells the operator to read the
// commit again. Running publish-check-run again is not that read: it would
// post every run the first pass decided was needed. This command is the read
// on its own, so the state of a publish can be looked at without a token that
// could change it, and it builds only the checks:read reconciler for exactly
// that reason.
func githubReconcileCheckRuns(ctx context.Context, in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}
	publisherConfig, enabled, err := github.LoadCheckRunPublisherConfigFromEnv(config.Mode)
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing has no repository: set %s",
			github.EnvCheckRunRepository)
	}

	var document checkRunPublishInput
	decoder := json.NewDecoder(io.LimitReader(in, maxCheckRunPublishBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read task outcomes: %w", err)
	}
	if decoder.More() {
		return errors.New("read task outcomes: trailing content after the document")
	}
	if decoder.InputOffset() > maxCheckRunPublishBytes {
		return fmt.Errorf("read task outcomes: larger than %d bytes", maxCheckRunPublishBytes)
	}

	planned, err := planCheckRuns(config.Mode, config.Correspondence, document)
	if err != nil {
		return fmt.Errorf("plan check runs: %w", err)
	}

	reconciler, err := github.NewCheckRunReconciler(github.CheckRunReconcilerConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}
	commit := planned[0].Run.HeadSHA
	published, err := reconciler.PublishedRuns(ctx, commit)
	if err != nil {
		return fmt.Errorf("read the check runs already on %s: %w", commit, err)
	}
	report, err := surveyCheckRunPlan(planned, published)
	if err != nil {
		return err
	}
	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(report); err != nil {
		return fmt.Errorf("write the reconciliation: %w", err)
	}
	// The report is written before the verdict is returned, so a conflict
	// is never carried by an exit status alone. A conflict is the answer
	// rather than an error in the inputs, and it is the one state that
	// does not resolve itself: a needed run is posted and a run in flight
	// finishes, but a run this plane cannot account for waits for a
	// person.
	if report.Conflict > 0 {
		conflicting := make([]string, 0, report.Conflict)
		for _, task := range report.Tasks {
			if task.Decision == decisionToken(github.PublishConflicts) {
				conflicting = append(conflicting, task.Task)
			}
		}
		return fmt.Errorf("%d of %d tasks have a check run on %s that this publish cannot account for: %s",
			report.Conflict, len(report.Tasks), commit, strings.Join(conflicting, ", "))
	}
	return nil
}

// maxRequiredCheckBytes bounds the gate document this reads. A repository's
// required-context list is a few dozen short strings, so this is room to spare
// and still a refusal rather than an unbounded read.
const maxRequiredCheckBytes = 64 << 10

// requiredCheckInput is the wire shape this command reads: the contexts branch
// protection requires today. It is a document rather than a fetch for the same
// reason the shadow comparison is: the plan is read before anything is changed,
// and a command that went and collected the gate itself would be one step from
// changing it.
type requiredCheckInput struct {
	Required []string `json:"required"`
}

// evidenceGateInput is several pull requests' comparisons, the threshold they
// are read at, and the gate as it stands today.
//
// It is its own shape rather than a "required" key added to shadowEvidenceInput,
// because both commands set DisallowUnknownFields and widening the evidence
// document would have `ra8ci github shadow-evidence` quietly accept a gate list
// it never reads. The evidence half is the same type, so the two documents
// cannot drift apart.
type evidenceGateInput struct {
	Threshold int                     `json:"threshold"`
	Commits   []shadowComparisonInput `json:"commits"`
	Required  []string                `json:"required"`
}

// githubEvidenceGate plans the gate from the shadow evidence rather than from
// the mode alone.
//
// `ra8ci github required-checks` plans for every task the declared
// correspondence covers, which is the mode's answer: a deployment flipped to
// authoritative on its first morning would propose requiring tasks nothing has
// ever compared. This command reads the same gate document, grades the pull
// requests behind it, and proposes only the tasks the evidence backs at the
// stated threshold. Everything it holds back is named with its reason.
//
// It plans; it changes nothing, here or on GitHub.
func githubEvidenceGate(in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}

	var document evidenceGateInput
	decoder := json.NewDecoder(io.LimitReader(in, maxShadowEvidenceBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read evidence gate document: %w", err)
	}
	if decoder.More() {
		return errors.New("read evidence gate document: trailing content after the document")
	}
	if decoder.InputOffset() > maxShadowEvidenceBytes {
		return fmt.Errorf("read evidence gate document: larger than %d bytes", maxShadowEvidenceBytes)
	}
	// Stated, never defaulted, for the reason shadow-evidence states it:
	// how many pull requests are representative is the operator's
	// judgement, and this command spends that judgement on a gate.
	if document.Threshold < 1 {
		return errors.New("read evidence gate document: no threshold stated")
	}
	if len(document.Commits) == 0 {
		return errors.New("read evidence gate document: no commits to accumulate")
	}

	reports, _, err := gradeShadowCommits(config, document.Commits)
	if err != nil {
		return err
	}
	evidence, err := github.AccumulateShadowEvidence(reports)
	if err != nil {
		return fmt.Errorf("accumulate shadow evidence: %w", err)
	}
	readiness, err := evidence.Readiness(document.Threshold)
	if err != nil {
		return fmt.Errorf("read shadow evidence at threshold %d: %w", document.Threshold, err)
	}
	plan, err := github.PlanRequiredChecksFromEvidence(config.Mode, readiness, document.Required)
	if err != nil {
		return fmt.Errorf("plan required checks from the evidence: %w", err)
	}

	withheld := make([]map[string]any, 0, len(plan.Withheld))
	for _, task := range plan.Withheld {
		withheld = append(withheld, map[string]any{
			"task":             task.Task,
			"context":          task.Context,
			"reason":           task.Reason.String(),
			"already_required": task.AlreadyRequired,
		})
	}
	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(map[string]any{
		"mode":             config.Mode.String(),
		"may_block_merges": config.Mode == github.ModeAuthoritative,
		"catalog_digest":   loaded.Digest(),
		"threshold":        plan.Threshold,
		"settled":          readiness.Settled(),
		"no_change":        plan.Plan.NoChange(),
		"add":              emptyWhenNil(plan.Plan.Add),
		"remove":           emptyWhenNil(plan.Plan.Remove),
		"keep":             emptyWhenNil(plan.Plan.Keep),
		"foreign":          emptyWhenNil(plan.Plan.Foreign),
		"withheld":         withheld,
	}); err != nil {
		return fmt.Errorf("write evidence gate plan: %w", err)
	}
	// The plan is written before the verdict is returned, the
	// shadow-compare convention: a caller reading only the exit status
	// must not be able to get a clean one from a plan nobody could read.
	// A held-back task is not an error in the inputs, it is the answer,
	// and the exit status says so because a pipeline that pipes this into
	// a gate change must not treat a partially-backed plan as a finished
	// one.
	if plan.Withholds() {
		return fmt.Errorf("the evidence does not back %d of the covered tasks at threshold %d",
			len(plan.Withheld), plan.Threshold)
	}
	return nil
}

// githubRequiredChecks reports what branch protection should require, given
// what it requires today. It plans; it changes nothing, here or on GitHub.
//
// The mode comes from the environment, like every other check-run command, and
// it decides the whole answer: a shadow deployment plans no additions at all,
// because a shadow run reports neutral whatever the task did and a required
// check satisfied by a failing task is worse than no gate.
// gradeShadowCommits collects and grades one evidence document's commits
// through the one declared correspondence, and reports per commit what that
// commit did not exercise.
//
// Both commands that read several pull requests' comparisons go through this,
// so evidence read for a readiness answer and evidence read for a gate plan are
// graded the same way rather than by two loops that can drift apart.
func gradeShadowCommits(config github.CheckRunEnvConfig, commits []shadowComparisonInput) ([]github.ShadowReport, []map[string]any, error) {
	reports := make([]github.ShadowReport, 0, len(commits))
	notExercised := make([]map[string]any, 0, len(commits))
	for i, commit := range commits {
		plane := make([]github.PlaneOutcome, 0, len(commit.Plane))
		for _, outcome := range commit.Plane {
			plane = append(plane, github.PlaneOutcome{
				Task: outcome.Task, HeadSHA: outcome.HeadSHA, Observed: outcome.Observed,
			})
		}
		actions := make([]github.ActionsOutcome, 0, len(commit.Actions))
		for _, outcome := range commit.Actions {
			actions = append(actions, github.ActionsOutcome{
				Job: outcome.Job, HeadSHA: outcome.HeadSHA, Conclusion: outcome.Conclusion,
			})
		}
		// Every commit is collected through the one declared
		// correspondence. Evidence graded under two different statements
		// of which job covers which task is not one body of evidence.
		collection, err := config.Correspondence.Collect(plane, actions)
		if err != nil {
			return nil, nil, fmt.Errorf("collect shadow observations for commit %d: %w", i+1, err)
		}
		report, err := github.CompareShadowRun(collection.Observations)
		if err != nil {
			return nil, nil, fmt.Errorf("compare shadow run for commit %d: %w", i+1, err)
		}
		reports = append(reports, report)
		// A task a commit did not exercise is not evidence for that
		// task and is not accumulated as any. It is reported per commit
		// so a task that reads as under-observed can be explained
		// without going back to the inputs.
		notExercised = append(notExercised, map[string]any{
			"head_sha":      report.HeadSHA,
			"not_exercised": emptyWhenNil(collection.NotRun),
			"comparisons":   len(report.Comparisons),
			"clean":         report.Clean(),
		})
	}
	return reports, notExercised, nil
}

// githubShadowEvidence grades several pull requests through the declared
// correspondence and reports which tasks the evidence would let a required
// check move for.
//
// #1481 holds that move until conclusions have been "compared against Actions
// over representative pull requests", which `github shadow-compare` cannot
// answer because it grades one commit. This is the same grading, accumulated.
func githubShadowEvidence(in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}

	var document shadowEvidenceInput
	decoder := json.NewDecoder(io.LimitReader(in, maxShadowEvidenceBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read shadow evidence: %w", err)
	}
	if decoder.More() {
		return errors.New("read shadow evidence: trailing content after the document")
	}
	if decoder.InputOffset() > maxShadowEvidenceBytes {
		return fmt.Errorf("read shadow evidence: larger than %d bytes", maxShadowEvidenceBytes)
	}
	// The threshold is stated, never defaulted. How many pull requests
	// are "representative" is the operator's judgement, and a default
	// here would answer a question nobody asked while looking like an
	// answer to the one they did.
	if document.Threshold < 1 {
		return errors.New("read shadow evidence: no threshold stated")
	}
	if len(document.Commits) == 0 {
		return errors.New("read shadow evidence: no commits to accumulate")
	}

	reports, notExercised, err := gradeShadowCommits(config, document.Commits)
	if err != nil {
		return err
	}

	evidence, err := github.AccumulateShadowEvidence(reports)
	if err != nil {
		return fmt.Errorf("accumulate shadow evidence: %w", err)
	}
	readiness, err := evidence.Readiness(document.Threshold)
	if err != nil {
		return fmt.Errorf("read shadow evidence at threshold %d: %w", document.Threshold, err)
	}

	tasks := make([]map[string]any, 0, len(evidence.Tasks))
	for _, task := range evidence.Tasks {
		tasks = append(tasks, map[string]any{
			"task":                task.Task,
			"observed":            task.Observed,
			"graded":              task.Graded,
			"agreed":              task.Agreed,
			"divergent":           task.Divergent,
			"conflicting":         task.Conflicting,
			"indeterminate":       task.Indeterminate,
			"conflicting_commits": emptyWhenNil(task.ConflictingCommits),
		})
	}
	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(map[string]any{
		"mode":             config.Mode.String(),
		"may_block_merges": config.Mode == github.ModeAuthoritative,
		"catalog_digest":   loaded.Digest(),
		"threshold":        readiness.Threshold,
		"settled":          readiness.Settled(),
		"commits":          notExercised,
		"tasks":            tasks,
		"ready":            emptyWhenNil(readiness.Ready),
		"conflicting":      emptyWhenNil(readiness.Conflicting),
		"insufficient":     emptyWhenNil(readiness.Insufficient),
	}); err != nil {
		return fmt.Errorf("write shadow evidence: %w", err)
	}
	// The page is written before the verdict is returned, the
	// shadow-compare convention: a caller reading only the exit status
	// must not be able to get a settled one from an answer nobody could
	// read.
	if !readiness.Settled() {
		return fmt.Errorf("shadow evidence is not settled at threshold %d: %d conflicting, %d insufficient",
			readiness.Threshold, len(readiness.Conflicting), len(readiness.Insufficient))
	}
	return nil
}

func githubRequiredChecks(in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	names := loaded.Names()
	config, enabled, err := github.LoadCheckRunConfigFromEnv(names)
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}

	var document requiredCheckInput
	decoder := json.NewDecoder(io.LimitReader(in, maxRequiredCheckBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read the required contexts: %w", err)
	}
	if decoder.More() {
		return errors.New("read the required contexts: trailing content after the document")
	}
	if decoder.InputOffset() > maxRequiredCheckBytes {
		return fmt.Errorf("read the required contexts: larger than %d bytes", maxRequiredCheckBytes)
	}

	// The gate is planned for the tasks the declared correspondence covers,
	// not for the whole catalog. A task with no Actions job to compare
	// against has no shadow evidence behind it, and #1481 holds the
	// required-check move until that evidence exists.
	covered := config.Correspondence.Tasks()
	plan, err := github.PlanRequiredChecks(config.Mode, covered, document.Required)
	if err != nil {
		return fmt.Errorf("plan the required checks: %w", err)
	}

	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	return encoder.Encode(map[string]any{
		"mode":             config.Mode.String(),
		"may_block_merges": config.Mode == github.ModeAuthoritative,
		"catalog_digest":   loaded.Digest(),
		"planned_tasks":    len(covered),
		"no_change":        plan.NoChange(),
		"add":              emptyWhenNil(plan.Add),
		"remove":           emptyWhenNil(plan.Remove),
		"keep":             emptyWhenNil(plan.Keep),
		"foreign":          emptyWhenNil(plan.Foreign),
	})
}

// maxGateRequestBytes bounds the branch document `gate` reads. One branch name
// is tens of bytes, so this is room to spare and still a refusal rather than an
// unbounded read of whatever is piped in.
const maxGateRequestBytes = 4 << 10

// gateBranchInput is the wire shape `gate` reads: which protected branch's
// requirements to report.
type gateBranchInput struct {
	Branch string `json:"branch"`
}

// githubGate reports the status check contexts branch protection requires on
// one branch today, as exactly the document `required-checks` reads, so the two
// compose:
//
//	ra8ci github gate <<<'{"branch":"main"}' | ra8ci github required-checks
//
// It reads GitHub and changes nothing there. Fetching lives in its own
// subcommand so that `required-checks`, the command whose output an operator
// reads before the gate is touched, still speaks to nothing.
func githubGate(ctx context.Context, in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}
	publisherConfig, enabled, err := github.LoadCheckRunPublisherConfigFromEnv(config.Mode)
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing has no repository: set %s",
			github.EnvCheckRunRepository)
	}

	var document gateBranchInput
	decoder := json.NewDecoder(io.LimitReader(in, maxGateRequestBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read the branch to report: %w", err)
	}
	if decoder.More() {
		return errors.New("read the branch to report: trailing content after the document")
	}
	if decoder.InputOffset() > maxGateRequestBytes {
		return fmt.Errorf("read the branch to report: larger than %d bytes", maxGateRequestBytes)
	}
	// The branch is named, never defaulted. The gate that matters is the one
	// on the branch pull requests merge into, and a default would read some
	// other branch's protection and report it as the gate.
	if document.Branch == "" {
		return errors.New("read the branch to report: no branch named")
	}

	reader, err := github.NewRequiredCheckReader(github.RequiredCheckReaderConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}
	required, err := reader.RequiredContexts(ctx, document.Branch)
	if err != nil {
		return fmt.Errorf("read the gate on %s: %w", document.Branch, err)
	}

	// The output is the next command's input and nothing else.
	// required-checks refuses a field it does not know, so an echoed branch
	// name would break the pipe this command exists for. It is encoded as
	// requiredCheckInput itself, so the two cannot drift apart.
	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	return encoder.Encode(requiredCheckInput{Required: emptyWhenNil(required)})
}

// maxActionsRunRequestBytes bounds the document `actions-run` reads. It carries
// one run number and this plane's own outcomes for one commit, which is a few
// kilobytes across the whole catalog.
const maxActionsRunRequestBytes = 256 << 10

// actionsRunRequest is the wire shape `actions-run` reads: which workflow run
// to collect, and what this plane observed on the same commit.
type actionsRunRequest struct {
	RunID int64               `json:"run_id"`
	Plane []planeOutcomeInput `json:"plane"`
}

// githubActionsRun reads one completed workflow run's job conclusions and
// writes exactly the document `shadow-compare` reads, so the two compose:
//
//	ra8ci github actions-run < observations.json | ra8ci github shadow-compare
//
// The plane half passes through verbatim and is never fetched. What this plane
// observed for a commit is the caller's statement, the same division
// shadow-compare draws by reading both sides rather than gathering either, and
// a command that collected the plane side itself would be making that statement
// silently.
//
// The run's attempt travels with the outcomes in actions_run. A re-run answers
// the same run number with different conclusions, so evidence that does not
// name the attempt it came from cannot be checked against the run a second
// reader sees.
//
// A refused read writes nothing. A document with the Actions half missing
// would grade every covered task as indeterminate, which reads as a comparison
// nobody made rather than a read that did not happen.
func githubActionsRun(ctx context.Context, in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}
	publisherConfig, enabled, err := github.LoadCheckRunPublisherConfigFromEnv(config.Mode)
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing has no repository: set %s",
			github.EnvCheckRunRepository)
	}

	// The document is read and refused before a reader exists, so a bad
	// document is reported as a bad document rather than as a failure to
	// reach GitHub.
	var document actionsRunRequest
	decoder := json.NewDecoder(io.LimitReader(in, maxActionsRunRequestBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read the run to collect: %w", err)
	}
	if decoder.More() {
		return errors.New("read the run to collect: trailing content after the document")
	}
	if decoder.InputOffset() > maxActionsRunRequestBytes {
		return fmt.Errorf("read the run to collect: larger than %d bytes", maxActionsRunRequestBytes)
	}
	if document.RunID <= 0 {
		return errors.New("read the run to collect: no workflow run named")
	}
	if len(document.Plane) == 0 {
		return errors.New("read the run to collect: no plane outcomes to compare against")
	}

	reader, err := github.NewActionsOutcomeReader(github.ActionsOutcomeReaderConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}
	run, err := reader.Outcomes(ctx, document.RunID)
	if err != nil {
		return fmt.Errorf("collect workflow run %d: %w", document.RunID, err)
	}

	// The output is the next command's input and nothing else. It is
	// encoded as shadowComparisonInput itself rather than a lookalike map,
	// so the two cannot drift apart.
	actions := make([]actionsOutcomeInput, 0, len(run.Outcomes))
	for _, outcome := range run.Outcomes {
		actions = append(actions, actionsOutcomeInput{
			Job: outcome.Job, HeadSHA: outcome.HeadSHA, Conclusion: outcome.Conclusion,
		})
	}
	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	return encoder.Encode(shadowComparisonInput{
		ActionsRun: &actionsRunAnchor{RunID: run.RunID, Attempt: run.Attempt, HeadSHA: run.HeadSHA},
		Plane:      document.Plane,
		Actions:    actions,
	})
}

// maxPullRequestRequestBytes bounds the document `pull-request` reads. It
// carries one pull request number, so this is room to spare and still a
// refusal rather than an unbounded read of whatever is piped in.
const maxPullRequestRequestBytes = 4 << 10

// pullRequestRunsRequest is the wire shape `pull-request` reads: which pull
// request to look at.
type pullRequestRunsRequest struct {
	Number int `json:"number"`
}

// pullRequestRunsReport is what `pull-request` writes: where a pull request is
// and what Actions recorded there.
type pullRequestRunsReport struct {
	Number         int                     `json:"number"`
	HeadSHA        string                  `json:"head_sha"`
	BaseRef        string                  `json:"base_ref"`
	State          string                  `json:"state"`
	Merged         bool                    `json:"merged"`
	FromFork       bool                    `json:"from_fork"`
	HeadRepository string                  `json:"head_repository"`
	Gradable       int                     `json:"gradable"`
	Runs           []pullRequestRunsListed `json:"runs"`
}

// pullRequestRunsListed is one workflow run on the pull request's head.
type pullRequestRunsListed struct {
	RunID      int64  `json:"run_id"`
	Workflow   string `json:"workflow"`
	Attempt    int    `json:"attempt"`
	Event      string `json:"event"`
	Status     string `json:"status"`
	Conclusion string `json:"conclusion"`
	// Gradable says whether `actions-run` will accept this run. A run
	// still executing is listed and marked, never dropped.
	Gradable bool `json:"gradable"`
}

// githubPullRequestRuns answers where one pull request is and which workflow
// runs Actions recorded on its head, so gathering #1481's evidence over
// representative pull requests starts from a pull request number rather than
// from run IDs copied out of the web interface:
//
//	ra8ci github pull-request           # what is on PR 1589's head
//	ra8ci github actions-run < obs.json # grade one of the run IDs it named
//
// It reads two things with two narrow tokens: the pull request through
// pull_requests:read and the commit's runs through actions:read. Neither token
// grows a second permission to save a hop, which is the seam the pull-request
// reader was built with.
//
// It never grades. Which run on a pull request is the evidence, and whether
// the pull request is representative at all, are the operator's judgement:
// this command reports the state those judgements are made from, the same
// division `reconcile` draws when it surveys a commit without publishing to
// it.
//
// A pull request whose head carries no completed run is an answer, not a
// failure: it exits clean with gradable 0, because "CI has not finished here
// yet" is exactly what somebody choosing pull requests needs to be told.
func githubPullRequestRuns(ctx context.Context, in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}
	publisherConfig, enabled, err := github.LoadCheckRunPublisherConfigFromEnv(config.Mode)
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing has no repository: set %s",
			github.EnvCheckRunRepository)
	}

	// The document is read and refused before a reader exists, so a bad
	// document is reported as a bad document rather than as a failure to
	// reach GitHub.
	var document pullRequestRunsRequest
	decoder := json.NewDecoder(io.LimitReader(in, maxPullRequestRequestBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read the pull request to look at: %w", err)
	}
	if decoder.More() {
		return errors.New("read the pull request to look at: trailing content after the document")
	}
	if decoder.InputOffset() > maxPullRequestRequestBytes {
		return fmt.Errorf("read the pull request to look at: larger than %d bytes", maxPullRequestRequestBytes)
	}
	if document.Number <= 0 {
		return errors.New("read the pull request to look at: no pull request named")
	}

	heads, err := github.NewPullRequestHeadReader(github.PullRequestHeadReaderConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}
	head, err := heads.Head(ctx, document.Number)
	if err != nil {
		return fmt.Errorf("read pull request %d: %w", document.Number, err)
	}

	runs, err := github.NewActionsOutcomeReader(github.ActionsOutcomeReaderConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}
	listed, err := runs.RunsOn(ctx, head.HeadSHA)
	if err != nil {
		return fmt.Errorf("read the workflow runs on %s: %w", head.HeadSHA, err)
	}

	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	return encoder.Encode(pullRequestRunsFrom(head, listed))
}

// pullRequestRunsFrom assembles the report. It is separate from the command so
// the shape can be pinned without a GitHub of any kind.
func pullRequestRunsFrom(head github.PullRequestHead, listed github.CommitWorkflowRuns) pullRequestRunsReport {
	report := pullRequestRunsReport{
		Number: head.Number, HeadSHA: head.HeadSHA, BaseRef: head.BaseRef,
		State: head.State, Merged: head.Merged, FromFork: head.FromFork,
		HeadRepository: head.HeadRepository,
		Runs:           []pullRequestRunsListed{},
	}
	for _, run := range listed.Runs {
		report.Runs = append(report.Runs, pullRequestRunsListed{
			RunID: run.ID, Workflow: run.Workflow, Attempt: run.Attempt,
			Event: run.Event, Status: run.Status, Conclusion: run.Conclusion,
			Gradable: run.Completed(),
		})
		if run.Completed() {
			report.Gradable++
		}
	}
	return report
}

// maxEvidenceRunRequestBytes bounds the document `evidence-run` reads: a pull
// request number and the name of one workflow.
const maxEvidenceRunRequestBytes = 4 << 10

// evidenceRunRequest is the wire shape `evidence-run` reads.
//
// It is its own shape rather than a `pull-request` document grown a field.
// Both commands set DisallowUnknownFields, so widening the shared one would
// have `pull-request` quietly accept a workflow name it never reads, and an
// operator would have no way to tell a selection that was made from one that
// was silently skipped.
type evidenceRunRequest struct {
	Number   int    `json:"number"`
	Workflow string `json:"workflow"`
}

// evidenceRunReport is what `evidence-run` writes: the one run on a pull
// request's head whose job conclusions are the Actions half of the shadow
// comparison, and enough of the pull request to judge whether it is
// representative.
//
// The head's state travels with the run deliberately. Job policy distrusts
// fork pull requests, and whether a merged pull request still says anything
// about the gate is the operator's call (#1589); making them run a second
// command to find that out invites the answer nobody looked up.
type evidenceRunReport struct {
	Number         int    `json:"number"`
	HeadSHA        string `json:"head_sha"`
	BaseRef        string `json:"base_ref"`
	State          string `json:"state"`
	Merged         bool   `json:"merged"`
	FromFork       bool   `json:"from_fork"`
	HeadRepository string `json:"head_repository"`
	Workflow       string `json:"workflow"`
	RunID          int64  `json:"run_id"`
	Attempt        int    `json:"attempt"`
	Event          string `json:"event"`
	Conclusion     string `json:"conclusion"`
}

// githubEvidenceRun answers, for one pull request and one workflow, which
// workflow run is the evidence. The run ID it writes is the one `actions-run`
// grades.
//
// It reads and selects; it changes nothing. The two reads hold one narrow
// permission each, pull_requests:read to find the head and actions:read to
// list the commit's runs, the seam #1589 drew and #1590 kept.
//
// A selection that cannot be made is a refusal, not a report. `reconcile`
// reports a conflict because the picture it was asked for still exists around
// it; here the answer IS the run, so writing a document with no run in it
// would hand a pipeline something that looks like evidence. The refusal names
// the commit, so `pull-request` on the same number shows the listing the
// selection was made from.
func githubEvidenceRun(ctx context.Context, in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}
	publisherConfig, enabled, err := github.LoadCheckRunPublisherConfigFromEnv(config.Mode)
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing has no repository: set %s",
			github.EnvCheckRunRepository)
	}

	// The document is read and refused before a reader exists, so a bad
	// ask is reported as a bad ask rather than as a failure to reach
	// GitHub.
	var document evidenceRunRequest
	decoder := json.NewDecoder(io.LimitReader(in, maxEvidenceRunRequestBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read the run to select: %w", err)
	}
	if decoder.More() {
		return errors.New("read the run to select: trailing content after the document")
	}
	if decoder.InputOffset() > maxEvidenceRunRequestBytes {
		return fmt.Errorf("read the run to select: larger than %d bytes", maxEvidenceRunRequestBytes)
	}
	if document.Number <= 0 {
		return errors.New("read the run to select: no pull request named")
	}
	if strings.TrimSpace(document.Workflow) == "" {
		return errors.New("read the run to select: no workflow named")
	}

	heads, err := github.NewPullRequestHeadReader(github.PullRequestHeadReaderConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}
	head, err := heads.Head(ctx, document.Number)
	if err != nil {
		return fmt.Errorf("read pull request %d: %w", document.Number, err)
	}

	runs, err := github.NewActionsOutcomeReader(github.ActionsOutcomeReaderConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}
	listed, err := runs.RunsOn(ctx, head.HeadSHA)
	if err != nil {
		return fmt.Errorf("read the workflow runs on %s: %w", head.HeadSHA, err)
	}

	selected, err := github.SelectEvidenceRun(listed, document.Workflow)
	if err != nil {
		return fmt.Errorf("select the evidence run on %s (pull request %d): %w",
			head.HeadSHA, document.Number, err)
	}

	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	return encoder.Encode(evidenceRunFrom(head, selected))
}

// evidenceRunFrom assembles the report. It is separate from the command so the
// shape can be pinned without a GitHub of any kind.
func evidenceRunFrom(head github.PullRequestHead, selected github.CommitWorkflowRun) evidenceRunReport {
	return evidenceRunReport{
		Number: head.Number, HeadSHA: head.HeadSHA, BaseRef: head.BaseRef,
		State: head.State, Merged: head.Merged, FromFork: head.FromFork,
		HeadRepository: head.HeadRepository,
		Workflow:       selected.Workflow, RunID: selected.ID, Attempt: selected.Attempt,
		Event: selected.Event, Conclusion: selected.Conclusion,
	}
}

// emptyWhenNil renders an empty list as [] rather than null, so a reader
// diffing two plans sees an empty section instead of a missing one.
func emptyWhenNil(values []string) []string {
	if values == nil {
		return []string{}
	}
	return values
}

// githubShadowConfig reports the check-run configuration this process would
// publish with. It reads the environment and the catalog and speaks to nobody:
// the whole point of shadow mode is that the decision to move onto the merge
// gate is made from evidence, so the command that shows what is configured
// must not itself publish a run.
//
// The mode is taken from the environment only. A mode on the command line
// would be a second place to say it, and the one that matters is the mode the
// running process was deployed with.
func githubShadowConfig(out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	names := loaded.Names()
	config, enabled, err := github.LoadCheckRunConfigFromEnv(names)
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}

	covered := config.Correspondence.Tasks()
	declared := make(map[string]bool, len(covered))
	pairs := make([]map[string]string, 0, len(covered))
	for _, task := range covered {
		job, found := config.Correspondence.Job(task)
		if !found {
			return fmt.Errorf("correspondence lists task %q without a job", task)
		}
		declared[task] = true
		pairs = append(pairs, map[string]string{"task": task, "actions_job": job})
	}

	// The uncovered tasks are named, not counted. A plane outcome the
	// correspondence does not cover is refused rather than dropped, so an
	// operator shipping a declaration that misses a task gets a refused
	// comparison, and the list of names is the only thing that says which.
	uncovered := make([]string, 0, len(names))
	for _, name := range names {
		if !declared[name] {
			uncovered = append(uncovered, name)
		}
	}
	sort.Strings(uncovered)

	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	return encoder.Encode(map[string]any{
		"mode": config.Mode.String(),
		// A shadow run reports neutral whatever the task did, so it can
		// never hold a pull request. Saying so here keeps the answer to
		// "can this deployment block a merge" in the output rather than
		// in the reader's head.
		"may_block_merges": config.Mode == github.ModeAuthoritative,
		"catalog_digest":   loaded.Digest(),
		"catalog_tasks":    len(names),
		"covered_tasks":    len(covered),
		"correspondence":   pairs,
		"uncovered_tasks":  uncovered,
	})
}

// githubSessionCheck checks that the configured scale-set credentials can
// establish and cleanly close an official GitHub message session without
// consuming jobs.
func githubSessionCheck(ctx context.Context) error {
	config, enabled, err := github.LoadSessionConfigFromEnv()
	if err != nil {
		return err
	}
	if !enabled {
		return errors.New("GitHub scale-set integration is not configured")
	}
	session, err := github.OpenSession(ctx, config)
	if err != nil {
		return err
	}
	closeCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := session.Close(closeCtx); err != nil {
		return fmt.Errorf("close GitHub scale-set check session: %w", err)
	}
	return json.NewEncoder(os.Stdout).Encode(map[string]any{
		"connected": true, "owner": config.Owner, "scale_set_id": config.ScaleSetID,
		"max_runners": config.MaxRunners,
	})
}

func usageError(message string) int {
	fmt.Fprintln(os.Stderr, "ra8ci:", message)
	return 2
}

// backupCommand is invoked only by the isolated, privileged backup-monitor unit.
func backupCommand(ctx context.Context, args []string) error {
	if len(args) == 1 && args[0] == "keygen" {
		if err := scaler.CreateBackupSigningKeyPair(os.Getenv("RA8CI_BACKUP_SIGNING_KEY"), os.Getenv("RA8CI_BACKUP_PUBLIC_KEY_FILE")); err != nil {
			return fmt.Errorf("create backup signing key pair: %w", err)
		}
		return nil
	}
	if len(args) != 1 || args[0] != "refresh" {
		return errors.New("usage: ra8ci backup refresh|keygen")
	}
	config := scaler.BackupMonitorConfig{
		PgBackRestPath:   os.Getenv("RA8CI_PGBACKREST_PATH"),
		PrivateKeyPath:   os.Getenv("RA8CI_BACKUP_SIGNING_KEY"),
		RestoreDrillPath: os.Getenv("RA8CI_RESTORE_DRILL_RECEIPT"),
		AttestationPath:  os.Getenv("RA8CI_BACKUP_ATTESTATION_FILE"),
		ApprovalID:       os.Getenv("RA8CI_BACKUP_APPROVAL_ID"),
		Stanza:           os.Getenv("RA8CI_PGBACKREST_STANZA"),
	}
	if err := scaler.RefreshBackupAttestation(ctx, config); err != nil {
		return fmt.Errorf("refresh backup attestation: %w", err)
	}
	return nil
}

// maxPullRequestEvidenceBytes bounds the document `pull-request-evidence`
// reads. It carries this plane's own outcomes for several pull requests
// rather than one commit's, so it is the size of the evidence document it
// produces rather than of one comparison, and still a refusal rather than an
// unbounded read of whatever is piped in.
const maxPullRequestEvidenceBytes = 4 << 20

// pullRequestEvidenceRequest is the wire shape `pull-request-evidence` reads:
// which workflow carries the evidence, how many graded commits the readiness
// answer is being asked for, and what this plane observed on each pull
// request.
type pullRequestEvidenceRequest struct {
	Workflow     string                     `json:"workflow"`
	Threshold    int                        `json:"threshold"`
	PullRequests []pullRequestEvidenceEntry `json:"pull_requests"`
}

// pullRequestEvidenceEntry is one pull request and this plane's own outcomes
// on it. The head commit is deliberately absent: the whole point of naming a
// pull request is that the head is read rather than typed.
type pullRequestEvidenceEntry struct {
	Number int                 `json:"number"`
	Plane  []planeOutcomeInput `json:"plane"`
}

// gatheredPullRequest is one pull request's Actions half once GitHub has
// answered for it, beside the plane half the caller stated.
type gatheredPullRequest struct {
	Number   int
	Outcomes github.ActionsRunOutcomes
	Plane    []planeOutcomeInput
}

// githubPullRequestEvidence gathers #1481's evidence from pull request numbers
// and writes exactly the document `shadow-evidence` reads, so the whole
// readiness answer is one pipe:
//
//	ra8ci github pull-request-evidence < pulls.json | ra8ci github shadow-evidence
//
// Every piece of this already existed and none of them met. `pull-request`
// says where a pull request is, `evidence-run` picks the run, `actions-run`
// grades one run whose ID somebody already holds, and `shadow-evidence`
// accumulates comparisons somebody already assembled. Gathering evidence over
// a dozen representative pull requests meant running three commands per pull
// request and pasting run IDs and head SHAs between them by hand, which is
// exactly where a commit gets graded against the wrong run.
//
// It reads and never writes to GitHub. Three narrow tokens do the work,
// pull_requests:read for each head and actions:read for each commit's runs and
// job conclusions, the seam #1589 drew and #1590 kept.
func githubPullRequestEvidence(ctx context.Context, in io.Reader, out io.Writer) error {
	loaded, err := catalog.Load()
	if err != nil {
		return fmt.Errorf("load task catalog: %w", err)
	}
	config, enabled, err := github.LoadCheckRunConfigFromEnv(loaded.Names())
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing is not configured: set %s",
			github.EnvShadowCorrespondenceFile)
	}
	publisherConfig, enabled, err := github.LoadCheckRunPublisherConfigFromEnv(config.Mode)
	if err != nil {
		return err
	}
	if !enabled {
		return fmt.Errorf("GitHub check-run publishing has no repository: set %s",
			github.EnvCheckRunRepository)
	}

	// The document is read and refused before a reader exists, so a bad
	// ask is reported as a bad ask rather than as a failure to reach
	// GitHub.
	var document pullRequestEvidenceRequest
	decoder := json.NewDecoder(io.LimitReader(in, maxPullRequestEvidenceBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&document); err != nil {
		return fmt.Errorf("read the pull requests to gather: %w", err)
	}
	if decoder.More() {
		return errors.New("read the pull requests to gather: trailing content after the document")
	}
	if decoder.InputOffset() > maxPullRequestEvidenceBytes {
		return fmt.Errorf("read the pull requests to gather: larger than %d bytes", maxPullRequestEvidenceBytes)
	}
	if err := checkPullRequestEvidenceAsk(document); err != nil {
		return fmt.Errorf("read the pull requests to gather: %w", err)
	}

	heads, err := github.NewPullRequestHeadReader(github.PullRequestHeadReaderConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}
	runs, err := github.NewActionsOutcomeReader(github.ActionsOutcomeReaderConfig{
		APIBaseURL:     publisherConfig.APIBaseURL,
		AppClientID:    publisherConfig.AppClientID,
		InstallationID: publisherConfig.InstallationID,
		PrivateKeyFile: publisherConfig.PrivateKeyFile,
		Owner:          publisherConfig.Owner,
		Repository:     publisherConfig.Repository,
	})
	if err != nil {
		return err
	}

	// Every pull request is gathered before anything is written. A
	// document holding the pull requests that answered before the read
	// failed is evidence over a smaller set than the one that was asked
	// for, and nothing downstream could tell the two apart.
	gathered := make([]gatheredPullRequest, 0, len(document.PullRequests))
	for _, asked := range document.PullRequests {
		head, err := heads.Head(ctx, asked.Number)
		if err != nil {
			return fmt.Errorf("read pull request %d: %w", asked.Number, err)
		}
		if err := checkPlaneIsAboutTheHead(asked, head.HeadSHA); err != nil {
			return err
		}
		listed, err := runs.RunsOn(ctx, head.HeadSHA)
		if err != nil {
			return fmt.Errorf("read the workflow runs on %s (pull request %d): %w",
				head.HeadSHA, asked.Number, err)
		}
		selected, err := github.SelectEvidenceRun(listed, document.Workflow)
		if err != nil {
			return fmt.Errorf("select the evidence run on %s (pull request %d): %w",
				head.HeadSHA, asked.Number, err)
		}
		outcomes, err := runs.Outcomes(ctx, selected.ID)
		if err != nil {
			return fmt.Errorf("collect workflow run %d (pull request %d): %w",
				selected.ID, asked.Number, err)
		}
		gathered = append(gathered, gatheredPullRequest{
			Number: asked.Number, Outcomes: outcomes, Plane: asked.Plane,
		})
	}

	// The output is the next command's input and nothing else. It is
	// encoded as shadowEvidenceInput itself rather than a lookalike map,
	// so the two cannot drift apart.
	encoder := json.NewEncoder(out)
	encoder.SetIndent("", "  ")
	return encoder.Encode(pullRequestEvidenceDocument(document.Threshold, gathered))
}

// checkPullRequestEvidenceAsk refuses an ask nothing could be gathered from,
// before a token is minted.
//
// A pull request named twice is refused rather than gathered twice or
// silently collapsed: two entries for one number are two different statements
// about what this plane observed there, and the readiness threshold counts
// commits, so gathering both would let one pull request answer a threshold of
// two.
func checkPullRequestEvidenceAsk(document pullRequestEvidenceRequest) error {
	if strings.TrimSpace(document.Workflow) == "" {
		return errors.New("no workflow named")
	}
	if document.Threshold <= 0 {
		return errors.New("no readiness threshold stated")
	}
	if len(document.PullRequests) == 0 {
		return errors.New("no pull requests to gather")
	}
	seen := make(map[int]struct{}, len(document.PullRequests))
	for _, asked := range document.PullRequests {
		if asked.Number <= 0 {
			return errors.New("a pull request with no number")
		}
		if _, repeated := seen[asked.Number]; repeated {
			return fmt.Errorf("pull request %d named twice", asked.Number)
		}
		seen[asked.Number] = struct{}{}
		if len(asked.Plane) == 0 {
			return fmt.Errorf("pull request %d has no plane outcomes to compare against", asked.Number)
		}
	}
	return nil
}

// checkPlaneIsAboutTheHead refuses a plane half stated for a commit the pull
// request is not at.
//
// The Actions half is read from the head this command just looked up, so a
// plane half about some other commit would be a comparison between two
// different commits. Collect refuses that mismatch downstream, but only ever
// by naming two SHAs; this command is the only place that knows which pull
// request they came from, which is what somebody re-gathering the evidence
// needs to be told. The plane's own commit is never rewritten to the head:
// correcting it silently would turn a caller who graded the wrong commit into
// evidence.
func checkPlaneIsAboutTheHead(asked pullRequestEvidenceEntry, head string) error {
	for _, outcome := range asked.Plane {
		if !strings.EqualFold(outcome.HeadSHA, head) {
			return fmt.Errorf("pull request %d is at %s, but this plane's outcome for task %q is about %s",
				asked.Number, head, outcome.Task, outcome.HeadSHA)
		}
	}
	return nil
}

// pullRequestEvidenceDocument assembles the evidence document. It is separate
// from the command so the shape can be pinned without a GitHub of any kind.
//
// The pull request numbers do not survive into it. shadow-evidence reads
// commits, and widening the document to carry the number each comparison came
// from would change a shape shadow-evidence and evidence-gate both read, for a
// field neither of them grades; the run anchor and the head SHA are the link
// back to where a comparison was gathered.
func pullRequestEvidenceDocument(threshold int, gathered []gatheredPullRequest) shadowEvidenceInput {
	document := shadowEvidenceInput{
		Threshold: threshold,
		Commits:   make([]shadowComparisonInput, 0, len(gathered)),
	}
	for _, one := range gathered {
		actions := make([]actionsOutcomeInput, 0, len(one.Outcomes.Outcomes))
		for _, outcome := range one.Outcomes.Outcomes {
			actions = append(actions, actionsOutcomeInput{
				Job: outcome.Job, HeadSHA: outcome.HeadSHA, Conclusion: outcome.Conclusion,
			})
		}
		document.Commits = append(document.Commits, shadowComparisonInput{
			ActionsRun: &actionsRunAnchor{
				RunID: one.Outcomes.RunID, Attempt: one.Outcomes.Attempt, HeadSHA: one.Outcomes.HeadSHA,
			},
			Plane:   one.Plane,
			Actions: actions,
		})
	}
	return document
}
