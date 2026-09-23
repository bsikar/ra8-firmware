// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"regexp"
	"strings"
	"time"

	"github.com/actions/scaleset"
	"github.com/actions/scaleset/listener"
)

const maxGitHubPrivateKeyBytes = 64 << 10

var ownerName = regexp.MustCompile(`^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?$`)

// SessionConfig contains explicit identity for the official GitHub runner
// scale-set API. The private key is read from a protected file and is never
// accepted inline from an environment variable.
type SessionConfig struct {
	GitHubConfigURL string
	AppClientID     string
	InstallationID  int64
	PrivateKeyFile  string
	Owner           string
	ScaleSetID      int
	MaxRunners      int
}

type scaleSetAdminAPI interface {
	GenerateJitRunnerConfig(context.Context, *scaleset.RunnerScaleSetJitRunnerSetting, int) (*scaleset.RunnerScaleSetJitRunnerConfig, error)
	GetRunner(context.Context, int) (*scaleset.RunnerReference, error)
	GetRunnerByName(context.Context, string) (*scaleset.RunnerReference, error)
	RemoveRunner(context.Context, int64) error
}

// Session owns one official GitHub message session. Close must be called
// after Controller.Run returns so GitHub does not retain a stale session.
type Session struct {
	Client     listener.Client
	close      func(context.Context) error
	admin      scaleSetAdminAPI
	scaleSetID int
}

// OpenSession verifies the configured scale-set identity before creating its
// message session. It performs network requests; callers must invoke it only
// after the server's production scaler and admission policy are constructed.
func OpenSession(ctx context.Context, config SessionConfig) (*Session, error) {
	if ctx == nil || strings.TrimSpace(config.GitHubConfigURL) != config.GitHubConfigURL ||
		config.GitHubConfigURL == "" || config.AppClientID == "" || config.InstallationID <= 0 ||
		config.PrivateKeyFile == "" || !ownerName.MatchString(config.Owner) ||
		config.ScaleSetID <= 0 || config.MaxRunners < 0 || config.MaxRunners > 10000 {
		return nil, errors.New("invalid GitHub scale-set session configuration")
	}
	keyInfo, err := os.Lstat(config.PrivateKeyFile)
	if err != nil {
		return nil, fmt.Errorf("stat GitHub App private key: %w", err)
	}
	if !keyInfo.Mode().IsRegular() || keyInfo.Size() < 1 || keyInfo.Size() > maxGitHubPrivateKeyBytes {
		return nil, errors.New("GitHub App private key must be a bounded regular file")
	}
	if keyInfo.Mode().Perm()&0077 != 0 {
		return nil, errors.New("GitHub App private key file must not be accessible by group or others")
	}
	keyFile, err := os.Open(config.PrivateKeyFile)
	if err != nil {
		return nil, fmt.Errorf("read GitHub App private key: %w", err)
	}
	defer keyFile.Close()
	openedInfo, err := keyFile.Stat()
	if err != nil || !openedInfo.Mode().IsRegular() || !os.SameFile(keyInfo, openedInfo) ||
		openedInfo.Size() < 1 || openedInfo.Size() > maxGitHubPrivateKeyBytes || openedInfo.Mode().Perm()&0077 != 0 {
		return nil, errors.New("GitHub App private key changed or is not a private regular file")
	}
	privateKey, err := io.ReadAll(io.LimitReader(keyFile, maxGitHubPrivateKeyBytes+1))
	if err != nil || len(privateKey) < 1 || len(privateKey) > maxGitHubPrivateKeyBytes {
		return nil, errors.New("GitHub App private key exceeds the bounded file size")
	}
	auth := scaleset.GitHubAppAuth{ClientID: config.AppClientID,
		InstallationID: config.InstallationID, PrivateKey: string(privateKey)}
	if err := auth.Validate(); err != nil {
		return nil, fmt.Errorf("GitHub App credentials: %w", err)
	}
	api, err := scaleset.NewClientWithGitHubApp(scaleset.ClientWithGitHubAppConfig{
		GitHubConfigURL: config.GitHubConfigURL,
		GitHubAppAuth:   auth,
		SystemInfo: scaleset.SystemInfo{System: "ra8ci", ScaleSetID: config.ScaleSetID,
			Subsystem: "controller"},
	}, scaleset.WithRetryMax(3))
	if err != nil {
		return nil, fmt.Errorf("create GitHub scale-set API client: %w", err)
	}
	set, err := api.GetRunnerScaleSetByID(ctx, config.ScaleSetID)
	if err != nil {
		return nil, fmt.Errorf("verify configured GitHub scale set: %w", err)
	}
	if set == nil || set.ID != config.ScaleSetID || set.RunnerGroupID <= 0 || set.Name == "" {
		return nil, errors.New("GitHub returned a mismatched or incomplete runner scale set")
	}
	messageClient, err := api.MessageSessionClient(ctx, config.ScaleSetID, config.Owner)
	if err != nil {
		return nil, fmt.Errorf("open GitHub scale-set message session: %w", err)
	}
	session := messageClient.Session()
	if session.SessionID.String() == "00000000-0000-0000-0000-000000000000" ||
		session.Statistics == nil || session.RunnerScaleSet == nil ||
		session.RunnerScaleSet.ID != config.ScaleSetID {
		cleanupCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
		_ = messageClient.Close(cleanupCtx)
		cancel()
		return nil, errors.New("GitHub returned an invalid scale-set session")
	}
	return &Session{Client: messageClient, close: messageClient.Close, admin: api, scaleSetID: config.ScaleSetID}, nil
}

// Close deletes the remote message session; a nil session is rejected.
func (s *Session) Close(ctx context.Context) error {
	if s == nil || s.close == nil || ctx == nil {
		return errors.New("invalid GitHub message session")
	}
	return s.close(ctx)
}

// ControllerSession owns both the controller and its remote message session.
// Run always attempts bounded remote cleanup before returning.
type ControllerSession struct {
	controller *Controller
	session    *Session
}

// OpenController connects to the explicitly configured official scale set and
// constructs the durable, admission-checked controller around it.
func OpenController(ctx context.Context, config SessionConfig, inbox ReplayInbox,
	handler Handler, admission Admission, processTimeout time.Duration) (*ControllerSession, error) {
	session, err := OpenSession(ctx, config)
	if err != nil {
		return nil, err
	}
	controller, err := NewController(session.Client, inbox, handler, admission,
		config.ScaleSetID, config.MaxRunners, processTimeout)
	if err != nil {
		cleanupCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = session.Close(cleanupCtx)
		return nil, err
	}
	return &ControllerSession{controller: controller, session: session}, nil
}

// Run processes messages until cancellation or a controller error, then closes
// the GitHub session with an independent five-second cleanup deadline.
func (s *ControllerSession) Run(ctx context.Context) error {
	if s == nil || s.controller == nil || s.session == nil || ctx == nil {
		return errors.New("invalid GitHub controller session")
	}
	runErr := s.controller.Run(ctx)
	cleanupCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return errors.Join(runErr, s.session.Close(cleanupCtx))
}
