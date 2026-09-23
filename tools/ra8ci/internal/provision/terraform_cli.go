// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package provision contains the trusted Terraform/Ansible boundary used by
// the control plane. It never accepts backend configuration from a job.
package provision

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	maxTerraformBinaryBytes = 256 << 20
	maxTerraformStateBytes  = 16 << 20
	maxTerraformPlanBytes   = 64 << 20
	maxTerraformOutputBytes = 64 << 10
)

var terraformVersionPattern = regexp.MustCompile(`^[0-9]+\.[0-9]+\.[0-9]+$`)
var terraformSHA256Pattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

// TerraformConfig fixes the executable, module, provider lock, remote backend,
// and private runtime workspace. None of these fields may come from a job.
type TerraformConfig struct {
	BinaryPath           string
	BinarySHA256         string
	Version              string
	EnvironmentDirectory string
	StateDirectory       string
	PluginCacheDirectory string
	OperationTimeout     time.Duration
	Backend              HTTPBackendConfig
	AppRole              AppRoleConfig
}

// TerraformRuntime verifies the pinned executable before allowing a session.
type TerraformRuntime struct {
	config TerraformConfig
}

// TerraformSession carries one reservation-scoped backend and Vault token.
type TerraformSession struct {
	runtime       *TerraformRuntime
	reservationID string
	workspace     string
	environment   []string
}

// OpenTerraformRuntime validates the immutable Terraform executable and its
// exact version. It performs no provider or infrastructure operation.
func OpenTerraformRuntime(ctx context.Context, config TerraformConfig) (*TerraformRuntime, error) {
	if ctx == nil || !filepath.IsAbs(config.BinaryPath) ||
		!terraformSHA256Pattern.MatchString(config.BinarySHA256) ||
		!terraformVersionPattern.MatchString(config.Version) ||
		!filepath.IsAbs(config.EnvironmentDirectory) || !filepath.IsAbs(config.StateDirectory) ||
		!filepath.IsAbs(config.PluginCacheDirectory) {
		return nil, errors.New("invalid pinned Terraform runtime configuration")
	}
	timeout := config.OperationTimeout
	if timeout == 0 {
		timeout = 20 * time.Minute
	}
	if timeout < time.Second || timeout > 30*time.Minute {
		return nil, errors.New("Terraform operation timeout is outside bounded policy")
	}
	config.OperationTimeout = timeout
	binaryDigest, err := fileSHA256(config.BinaryPath, maxTerraformBinaryBytes)
	if err != nil || binaryDigest != config.BinarySHA256 {
		return nil, errors.New("Terraform executable digest differs from pinned configuration")
	}
	info, err := os.Stat(config.EnvironmentDirectory)
	if err != nil || !info.IsDir() {
		return nil, errors.New("Terraform environment directory is unavailable")
	}
	if filepath.Clean(config.EnvironmentDirectory) == filepath.Clean(config.StateDirectory) ||
		pathInside(config.EnvironmentDirectory, config.StateDirectory) {
		return nil, errors.New("Terraform runtime state must be outside the source environment")
	}
	runtime := &TerraformRuntime{config: config}
	versionCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	command := exec.CommandContext(versionCtx, config.BinaryPath, "version", "-json")
	command.Dir = config.EnvironmentDirectory
	output := &boundedTerraformBuffer{limit: maxTerraformOutputBytes}
	command.Stdout = output
	command.Stderr = io.Discard
	if err := command.Run(); err != nil {
		return nil, errors.New("pinned Terraform version probe failed")
	}
	var version struct {
		TerraformVersion string `json:"terraform_version"`
	}
	if err := json.Unmarshal(output.data, &version); err != nil || version.TerraformVersion != config.Version {
		return nil, errors.New("Terraform version differs from pinned configuration")
	}
	clear(output.data)
	return runtime, nil
}

// WithSession creates a private reservation workspace, obtains a short-lived
// Vault token, and closes the token lifetime after the callback returns.
func (runtime *TerraformRuntime) WithSession(ctx context.Context, reservationID string, operation func(*TerraformSession) error) (result error) {
	if runtime == nil || ctx == nil || !store.ValidID(reservationID) || operation == nil {
		return errors.New("invalid Terraform session request")
	}
	root := runtime.config.StateDirectory
	if err := secureDirectory(root); err != nil {
		return fmt.Errorf("prepare Terraform state directory: %w", err)
	}
	workspace := filepath.Join(root, reservationID)
	if err := secureDirectory(workspace); err != nil {
		return fmt.Errorf("prepare reservation workspace: %w", err)
	}
	backend := runtime.config.Backend
	backend.ReservationID = reservationID
	backendEnvironment, err := HTTPBackendEnvironment(backend)
	if err != nil {
		return err
	}
	token, err := LoginAppRole(ctx, runtime.config.AppRole)
	if err != nil {
		return err
	}
	defer func() {
		revokeCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 3*time.Second)
		revokeErr := token.Revoke(revokeCtx)
		cancel()
		token.Clear()
		result = errors.Join(result, revokeErr)
	}()
	tokenEnvironment, err := token.TerraformEnvironment()
	if err != nil {
		return err
	}
	baseEnvironment := terraformBaseEnvironment(workspace)
	baseEnvironment = append(baseEnvironment,
		"TF_DATA_DIR="+filepath.Join(workspace, "tfdata"),
		"TF_PLUGIN_CACHE_DIR="+runtime.config.PluginCacheDirectory)
	overlay := append(backendEnvironment, tokenEnvironment...)
	environment, err := OverlayEnvironment(baseEnvironment, overlay)
	if err != nil {
		return err
	}
	if err := secureDirectory(filepath.Join(workspace, "tfdata")); err != nil {
		return fmt.Errorf("prepare Terraform data directory: %w", err)
	}
	if err := secureDirectory(runtime.config.PluginCacheDirectory); err != nil {
		return fmt.Errorf("prepare Terraform plugin cache: %w", err)
	}
	session := &TerraformSession{runtime: runtime, reservationID: reservationID,
		workspace: workspace, environment: environment}
	result = operation(session)
	session.environment = nil
	return result
}

// Init initializes only the fixed environment using the committed provider
// lockfile. Backend settings arrive from the reservation-bound environment.
func (session *TerraformSession) Init(ctx context.Context) error {
	if err := session.validate(ctx); err != nil {
		return err
	}
	return session.run(ctx, nil, "init", "-input=false", "-lockfile=readonly", "-no-color")
}

// Plan creates one immutable operation-specific saved plan from a private
// variable file. The caller must persist its digest and apply intent first.
func (session *TerraformSession) Plan(ctx context.Context, operationID, variableFile string, destroy bool) (string, string, error) {
	if err := session.validate(ctx); err != nil || !store.ValidID(operationID) {
		return "", "", errors.New("invalid Terraform plan request")
	}
	opDirectory := filepath.Join(session.workspace, operationID)
	if err := secureDirectory(opDirectory); err != nil {
		return "", "", fmt.Errorf("prepare Terraform operation directory: %w", err)
	}
	if err := requirePrivateFileInside(session.workspace, variableFile, 1<<20); err != nil {
		return "", "", fmt.Errorf("validate Terraform variable file: %w", err)
	}
	planFile := filepath.Join(opDirectory, "saved.tfplan")
	planHandle, err := os.OpenFile(planFile, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		return "", "", errors.New("create private Terraform plan file")
	}
	if err := planHandle.Close(); err != nil {
		_ = os.Remove(planFile)
		return "", "", errors.New("close Terraform plan file")
	}
	args := []string{"plan", "-input=false", "-no-color", "-lock-timeout=30s",
		"-var-file=" + variableFile, "-out=" + planFile}
	if destroy {
		args = append(args, "-destroy")
	}
	if err := session.run(ctx, nil, args...); err != nil {
		_ = os.Remove(planFile)
		return "", "", err
	}
	if err := os.Chmod(planFile, 0o600); err != nil {
		return "", "", errors.New("protect Terraform saved plan")
	}
	digest, err := fileSHA256(planFile, maxTerraformPlanBytes)
	if err != nil {
		return "", "", fmt.Errorf("read Terraform saved plan digest: %w", err)
	}
	return planFile, digest, nil
}

// Apply applies only a private saved plan in the reservation workspace. It
// never retries a failed or interrupted apply; the caller must reconcile first.
func (session *TerraformSession) Apply(ctx context.Context, planFile string) error {
	if err := session.validate(ctx); err != nil {
		return err
	}
	if err := requirePrivateFileInside(session.workspace, planFile, maxTerraformPlanBytes); err != nil {
		return fmt.Errorf("validate Terraform saved plan: %w", err)
	}
	return session.run(ctx, nil, "apply", "-input=false", "-no-color", planFile)
}

// StatePull retrieves remote state through the reservation-bound backend.
func (session *TerraformSession) StatePull(ctx context.Context) ([]byte, error) {
	if err := session.validate(ctx); err != nil {
		return nil, err
	}
	output := &boundedTerraformBuffer{limit: maxTerraformStateBytes}
	if err := session.run(ctx, output, "state", "pull"); err != nil {
		clear(output.data)
		return nil, err
	}
	return output.data, nil
}

func (session *TerraformSession) run(ctx context.Context, stdout io.Writer, args ...string) error {
	if ctx == nil || session == nil || session.runtime == nil || len(args) == 0 {
		return errors.New("invalid Terraform command")
	}
	commandCtx, cancel := context.WithTimeout(ctx, session.runtime.config.OperationTimeout)
	defer cancel()
	command := exec.CommandContext(commandCtx, session.runtime.config.BinaryPath, args...)
	command.Dir = session.runtime.config.EnvironmentDirectory
	command.Env = session.environment
	command.Stdout = stdout
	if command.Stdout == nil {
		command.Stdout = io.Discard
	}
	command.Stderr = io.Discard
	command.WaitDelay = 2 * time.Second
	if err := command.Run(); err != nil {
		if errors.Is(commandCtx.Err(), context.DeadlineExceeded) {
			return errors.New("Terraform command exceeded its deadline")
		}
		return errors.New("Terraform command failed; reconcile state before retry")
	}
	return nil
}

func (session *TerraformSession) validate(ctx context.Context) error {
	if session == nil || session.runtime == nil || ctx == nil ||
		!store.ValidID(session.reservationID) || session.workspace == "" ||
		len(session.environment) == 0 {
		return errors.New("invalid Terraform session")
	}
	return ctx.Err()
}

func secureDirectory(directory string) error {
	if !filepath.IsAbs(directory) {
		return errors.New("directory path must be absolute")
	}
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return err
	}
	info, err := os.Lstat(directory)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("directory is not a real directory")
	}
	return os.Chmod(directory, 0o700)
}

func requirePrivateFileInside(root, file string, limit int64) error {
	if !filepath.IsAbs(root) || !filepath.IsAbs(file) || !pathInside(root, file) {
		return errors.New("Terraform file must be inside its reservation workspace")
	}
	info, err := os.Lstat(file)
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 ||
		info.Size() < 1 || info.Size() > limit || info.Mode().Perm()&0o077 != 0 {
		return errors.New("Terraform file must be private, bounded, and regular")
	}
	return nil
}

func pathInside(root, candidate string) bool {
	relative, err := filepath.Rel(filepath.Clean(root), filepath.Clean(candidate))
	return err == nil && relative != "." && relative != ".." &&
		!strings.HasPrefix(relative, ".."+string(filepath.Separator))
}

func fileSHA256(file string, limit int64) (string, error) {
	info, err := os.Lstat(file)
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 ||
		info.Size() < 1 || info.Size() > limit {
		return "", errors.New("file is not a bounded regular file")
	}
	handle, err := os.Open(file)
	if err != nil {
		return "", errors.New("file cannot be opened")
	}
	defer handle.Close()
	opened, err := handle.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) ||
		opened.Size() != info.Size() || opened.Size() > limit {
		return "", errors.New("file changed while being opened")
	}
	hash := sha256.New()
	count, err := io.Copy(hash, io.LimitReader(handle, limit+1))
	if err != nil || count != opened.Size() {
		return "", errors.New("file digest failed or changed")
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

type boundedTerraformBuffer struct {
	data  []byte
	limit int
}

func (buffer *boundedTerraformBuffer) Write(data []byte) (int, error) {
	if buffer == nil || buffer.limit < len(buffer.data) ||
		len(data) > buffer.limit-len(buffer.data) {
		return 0, errors.New("Terraform output limit exceeded")
	}
	buffer.data = append(buffer.data, data...)
	return len(data), nil
}

// terraformBaseEnvironment deliberately excludes the service process
// environment: Terraform providers and provisioner plugins must not inherit
// unrelated credentials (for example GitHub, database, or cloud tokens).
func terraformBaseEnvironment(workspace string) []string {
	return []string{
		"PATH=/usr/bin:/bin",
		"HOME=" + workspace,
		"LANG=C.UTF-8",
		"TF_IN_AUTOMATION=1",
	}
}
