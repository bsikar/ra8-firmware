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
		fmt.Fprintln(os.Stderr, "usage: ra8ci <task>|tasks [--digest|--json]|ascii [--check] [--all|PATH]|since [--all|FILE...]|final-newline [FILE...]|runner-clock [--repo OWNER/REPO] [--runs N] [--hours N]|tests-readme [--selftest]|inclusive-terminology-commits [--selftest]|server|agent|sync|backup refresh|keygen|board status|take [--class human|ci|agent]|checkpoint|extend|cancel|hil budget|verify-capture|db migrate|report slow|github check|github shadow|github shadow-compare|github publish-check-run|github required-checks|github gate|run submit|run status")
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
		return errors.New("usage: ra8ci github check|shadow|shadow-compare|publish-check-run|required-checks|gate")
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
	case "required-checks":
		return githubRequiredChecks(os.Stdin, os.Stdout)
	case "gate":
		return githubGate(ctx, os.Stdin, os.Stdout)
	default:
		return errors.New("usage: ra8ci github check|shadow|shadow-compare|publish-check-run|required-checks|gate")
	}
}

// maxShadowComparisonBytes bounds the observation document this reads. One
// commit's outcomes across the whole catalog are a few kilobytes, so this is
// room to spare and still a refusal rather than an unbounded read of whatever
// is piped in.
const maxShadowComparisonBytes = 256 << 10

// shadowComparisonInput is the wire shape this command reads. The field names
// are declared here rather than as tags on PlaneOutcome and ActionsOutcome,
// because a wire contract on those types would outlive this command and they
// were written as in-process values.
type shadowComparisonInput struct {
	Plane []struct {
		Task     string `json:"task"`
		HeadSHA  string `json:"head_sha"`
		Observed string `json:"observed"`
	} `json:"plane"`
	Actions []struct {
		Job        string `json:"job"`
		HeadSHA    string `json:"head_sha"`
		Conclusion string `json:"conclusion"`
	} `json:"actions"`
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

// githubPublishCheckRuns posts one check run per task outcome for one commit.
//
// This is the first ra8ci command that writes to GitHub. What it may write is
// configuration, not an argument: the mode comes from the environment, so a
// shadow deployment posts runs that report neutral and cannot hold a pull
// request, and moving onto the merge gate is a deployment change.
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
	for index, plan := range planned {
		id, err := publisher.Publish(ctx, plan.Run, plan.Summary)
		if err != nil {
			// A partial publish is not a failed publish. The runs
			// already posted are on the commit whatever this
			// command returns, so the count is reported rather
			// than left for the operator to guess.
			return fmt.Errorf("publish %s after %d of %d posted: %w", plan.Task, index, len(planned), err)
		}
		if _, err := fmt.Fprintf(out, "%d %s %s\n", id, plan.Run.Conclusion, plan.Run.Name); err != nil {
			return fmt.Errorf("report published check run: %w", err)
		}
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

// githubRequiredChecks reports what branch protection should require, given
// what it requires today. It plans; it changes nothing, here or on GitHub.
//
// The mode comes from the environment, like every other check-run command, and
// it decides the whole answer: a shadow deployment plans no additions at all,
// because a shadow run reports neutral whatever the task did and a required
// check satisfied by a failing task is worse than no gate.
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
