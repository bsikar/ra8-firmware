// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func currentBoardLeaseDirectory() (string, error) {
	config, err := os.UserConfigDir()
	if err != nil || config == "" {
		return "", errors.New("user configuration directory is unavailable")
	}
	appDirectory := filepath.Join(config, "ra8ci")
	if err := os.MkdirAll(appDirectory, 0o700); err != nil {
		return "", errors.New("create private ra8ci configuration directory")
	}
	info, err := os.Lstat(appDirectory)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm()&0o077 != 0 {
		return "", errors.New("ra8ci configuration directory must be private and not a symlink")
	}
	return filepath.Join(appDirectory, "board-leases"), nil
}

func writeBoardLeaseToken(directory string, token boardclient.LeaseToken) error {
	if !validBoardIDArgument(token.BoardID) || !store.ValidID(token.RequestID) ||
		!store.ValidID(token.LeaseID) || token.Generation == 0 || token.Version == 0 || token.ExpiresAt.IsZero() {
		return errors.New("invalid board lease token")
	}
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return errors.New("create private board lease directory")
	}
	info, err := os.Lstat(directory)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm()&0o077 != 0 {
		return errors.New("board lease directory must be a private, nonsymlink directory")
	}
	encoded, err := json.Marshal(token)
	if err != nil {
		return errors.New("encode board lease token")
	}
	defer clear(encoded)
	temporary, err := os.CreateTemp(directory, ".lease-*.tmp")
	if err != nil {
		return errors.New("create temporary board lease token")
	}
	tempPath := temporary.Name()
	defer func() { _ = os.Remove(tempPath) }()
	if err := temporary.Chmod(0o600); err != nil {
		_ = temporary.Close()
		return errors.New("protect temporary board lease token")
	}
	if _, err := temporary.Write(encoded); err != nil {
		_ = temporary.Close()
		return errors.New("write board lease token")
	}
	if err := temporary.Sync(); err != nil {
		_ = temporary.Close()
		return errors.New("sync board lease token")
	}
	if err := temporary.Close(); err != nil {
		return errors.New("close board lease token")
	}
	destination := filepath.Join(directory, token.BoardID+".json")
	if err := os.Rename(tempPath, destination); err != nil {
		return errors.New("atomically store board lease token")
	}
	return nil
}

func readBoardLeaseToken(directory, boardID string) (boardclient.LeaseToken, error) {
	if !validBoardIDArgument(boardID) {
		return boardclient.LeaseToken{}, errors.New("invalid board ID")
	}
	info, err := os.Lstat(directory)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm()&0o077 != 0 {
		return boardclient.LeaseToken{}, errors.New("board lease directory is unavailable or unsafe")
	}
	path := filepath.Join(directory, boardID+".json")
	info, err = os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 ||
		info.Size() <= 0 || info.Size() > 4096 || info.Mode().Perm()&0o077 != 0 {
		return boardclient.LeaseToken{}, errors.New("board lease token is unavailable or unsafe")
	}
	file, err := os.Open(path)
	if err != nil {
		return boardclient.LeaseToken{}, errors.New("open board lease token")
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) ||
		opened.Size() <= 0 || opened.Size() > 4096 || opened.Mode().Perm()&0o077 != 0 {
		return boardclient.LeaseToken{}, errors.New("board lease token changed or became unsafe")
	}
	raw, err := io.ReadAll(io.LimitReader(file, 4097))
	if err != nil || len(raw) == 0 || len(raw) > 4096 {
		return boardclient.LeaseToken{}, errors.New("read bounded board lease token")
	}
	defer clear(raw)
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	var token boardclient.LeaseToken
	if err := decoder.Decode(&token); err != nil {
		return boardclient.LeaseToken{}, errors.New("decode board lease token")
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return boardclient.LeaseToken{}, errors.New("board lease token has trailing data")
	}
	if token.BoardID != boardID || !store.ValidID(token.RequestID) || !store.ValidID(token.LeaseID) ||
		token.Generation == 0 || token.Version == 0 || token.ExpiresAt.IsZero() {
		return boardclient.LeaseToken{}, errors.New("board lease token does not match its board")
	}
	return token, nil
}

type boardLeaseCheckpointer interface {
	Checkpoint(context.Context, boardclient.LeaseToken) (board.Snapshot, error)
}

// checkpointBoardLease moves a yielded holder into drain state after it has
// reached an application-defined safe point. It does not claim the hardware
// is neutral or release the lease.
func checkpointBoardLease(ctx context.Context, client boardLeaseCheckpointer, directory, boardID string) (board.Snapshot, error) {
	if ctx == nil || client == nil {
		return board.Snapshot{}, errors.New("board checkpoint requires context and client")
	}
	token, err := readBoardLeaseToken(directory, boardID)
	if err != nil {
		return board.Snapshot{}, err
	}
	snapshot, err := client.Checkpoint(ctx, token)
	if err != nil {
		return board.Snapshot{}, err
	}
	if snapshot.BoardID != token.BoardID || snapshot.Phase != board.Draining || snapshot.Lease == nil ||
		snapshot.Lease.ID != token.LeaseID || snapshot.Lease.WaiterID != token.RequestID ||
		snapshot.Lease.Generation != token.Generation {
		return board.Snapshot{}, errors.New("server returned a checkpoint for another board lease or phase")
	}
	return snapshot, nil
}

type boardLeaseExtender interface {
	Extend(context.Context, boardclient.LeaseToken, time.Time, string) (board.Snapshot, error)
}

func extendBoardLease(ctx context.Context, client boardLeaseExtender, directory, boardID string,
	expiry time.Time, why string) (board.Snapshot, error) {
	if ctx == nil || client == nil {
		return board.Snapshot{}, errors.New("board lease extension requires context and client")
	}
	token, err := readBoardLeaseToken(directory, boardID)
	if err != nil {
		return board.Snapshot{}, err
	}
	snapshot, err := client.Extend(ctx, token, expiry, why)
	if err != nil {
		return board.Snapshot{}, err
	}
	if snapshot.BoardID != token.BoardID || snapshot.Lease == nil || snapshot.Lease.ID != token.LeaseID ||
		snapshot.Lease.WaiterID != token.RequestID || snapshot.Lease.Generation != token.Generation {
		return board.Snapshot{}, errors.New("server returned an extension for another board lease")
	}
	token.ExpiresAt = snapshot.Lease.ExpiresAt
	token.Version = snapshot.Version
	if err := writeBoardLeaseToken(directory, token); err != nil {
		return board.Snapshot{}, fmt.Errorf("lease was extended but local token could not be updated: %w", err)
	}
	return snapshot, nil
}

type boardLeaseHeartbeater interface {
	Heartbeat(context.Context, boardclient.LeaseToken) (board.Snapshot, boardclient.HolderLiveness, error)
}

// heartbeatBoardLease reports the local holder still alive using the lease
// token this machine already holds. A beat is evidence about the holder, never
// a request for more time: nothing here names a duration, and a deadline that
// came back later than the token records is refused rather than written down,
// because a liveness call that bought time would route around the reason and
// the class ceiling every extension is held to.
func heartbeatBoardLease(ctx context.Context, client boardLeaseHeartbeater, directory,
	boardID string) (board.Snapshot, boardclient.HolderLiveness, error) {
	if ctx == nil || client == nil {
		return board.Snapshot{}, boardclient.HolderLiveness{}, errors.New("board heartbeat requires context and client")
	}
	token, err := readBoardLeaseToken(directory, boardID)
	if err != nil {
		return board.Snapshot{}, boardclient.HolderLiveness{}, err
	}
	snapshot, liveness, err := client.Heartbeat(ctx, token)
	if err != nil {
		return board.Snapshot{}, boardclient.HolderLiveness{}, err
	}
	if snapshot.BoardID != token.BoardID || snapshot.Lease == nil || snapshot.Lease.ID != token.LeaseID ||
		snapshot.Lease.WaiterID != token.RequestID || snapshot.Lease.Generation != token.Generation {
		return board.Snapshot{}, boardclient.HolderLiveness{}, errors.New("server returned a heartbeat for another board lease")
	}
	if !liveness.Held || liveness.LeaseID != token.LeaseID {
		return board.Snapshot{}, boardclient.HolderLiveness{}, errors.New("server reported liveness for another board lease")
	}
	if snapshot.Lease.ExpiresAt.After(token.ExpiresAt) {
		return board.Snapshot{}, boardclient.HolderLiveness{}, errors.New("board heartbeat came back with a later deadline; a beat may not extend a lease")
	}
	token.Version = snapshot.Version
	token.ExpiresAt = snapshot.Lease.ExpiresAt
	if err := writeBoardLeaseToken(directory, token); err != nil {
		return board.Snapshot{}, boardclient.HolderLiveness{},
			fmt.Errorf("holder was reported alive but the local token could not be updated: %w", err)
	}
	return snapshot, liveness, nil
}

// boardLivenessLine is the CLI rendering of a liveness report. Durations go out
// as whole seconds and absent instants as null, so a board nobody has ever
// beaten for does not read as one last seen at the zero time.
type boardLivenessLine struct {
	Held            bool       `json:"held"`
	LeaseID         string     `json:"lease_id,omitempty"`
	Holder          string     `json:"holder,omitempty"`
	LastSeenAt      *time.Time `json:"last_seen_at"`
	Beat            bool       `json:"beat"`
	SilenceSeconds  int64      `json:"silence_seconds"`
	IntervalSeconds int64      `json:"interval_seconds"`
	NextBeatBy      *time.Time `json:"next_beat_by"`
	Overdue         bool       `json:"overdue"`
	ExpiresAt       *time.Time `json:"expires_at"`
	Explain         string     `json:"explain"`
}

func boardLivenessLineFrom(report boardclient.HolderLiveness) boardLivenessLine {
	line := boardLivenessLine{Held: report.Held, LeaseID: report.LeaseID, Holder: report.Holder,
		Beat: report.Beat, SilenceSeconds: int64(report.Silence / time.Second),
		IntervalSeconds: int64(report.Interval / time.Second), Overdue: report.Overdue,
		Explain: report.Explain}
	if !report.LastSeenAt.IsZero() {
		seen := report.LastSeenAt.UTC()
		line.LastSeenAt = &seen
	}
	if !report.NextBeatBy.IsZero() {
		next := report.NextBeatBy.UTC()
		line.NextBeatBy = &next
	}
	if !report.ExpiresAt.IsZero() {
		expiry := report.ExpiresAt.UTC()
		line.ExpiresAt = &expiry
	}
	return line
}
