// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
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
	"strings"
	"syscall"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/agent"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/neutral"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/scaler"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/server"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
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
		fmt.Fprintln(os.Stderr, "usage: ra8ci <task>|tasks|server|agent|sync|backup refresh|keygen|board status|take|cancel|db migrate|report slow|run submit|run status")
		return 2
	}
	var err error
	switch args[0] {
	case "tasks":
		if len(args) != 1 {
			return usageError("tasks takes no arguments")
		}
		var cat *catalog.Catalog
		cat, err = catalog.Load()
		if err == nil {
			for _, name := range cat.Names() {
				fmt.Fprintln(os.Stdout, name)
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
	case "sync":
		if len(args) != 1 {
			return usageError("sync takes no arguments")
		}
		err = syncLocalRuns(ctx)
	case "backup":
		err = backupCommand(ctx, args[1:])
	case "board":
		err = boardCommand(ctx, args[1:])
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
	if err := task.ValidateArguments(args[1:]); err != nil {
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
		DeadlineSeconds: task.DeadlineSeconds, Args: append([]string(nil), args[1:]...)}
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
	result, err := executor.Run(ctx, root, task, os.Stdout, os.Stderr)
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
	cert, err := tls.LoadX509KeyPair(certPath, keyPath)
	if err != nil {
		return fmt.Errorf("load TLS identity: %w", err)
	}
	caPEM, err := os.ReadFile(caPath)
	if err != nil {
		return fmt.Errorf("load client CA: %w", err)
	}
	clientCAs := x509.NewCertPool()
	if !clientCAs.AppendCertsFromPEM(caPEM) {
		return errors.New("client CA has no trusted certificate")
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
				stop()
				if maintenanceErr != nil {
					maintenanceFailed <- fmt.Errorf("reap expired agent assignments: %w", maintenanceErr)
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
	config := agent.Config{
		ServerURL: os.Getenv("RA8CI_SERVER_URL"),
		CAFile:    os.Getenv("RA8CI_SERVER_CA"),
		CertFile:  os.Getenv("RA8CI_AGENT_CERT"),
		KeyFile:   os.Getenv("RA8CI_AGENT_KEY"),
		Root:      os.Getenv("RA8CI_AGENT_ROOT"),
	}
	if config.ServerURL == "" || config.CAFile == "" || config.CertFile == "" ||
		config.KeyFile == "" || config.Root == "" {
		return errors.New("agent requires RA8CI_SERVER_URL, RA8CI_SERVER_CA, RA8CI_AGENT_CERT, RA8CI_AGENT_KEY, and RA8CI_AGENT_ROOT")
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
