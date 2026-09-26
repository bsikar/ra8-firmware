// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"time"
)

// ParseLatestFullBackupInfo reads pgBackRest's JSON info format, requiring
// exactly one requested stanza and selecting its newest completed full backup.
//
// A full backup pgBackRest itself flags as errored is not a completed backup
// and is skipped. pgBackRest sets "error" on a backup whose copy finished
// with file-level failures, and such a backup still carries type "full" and
// a stop timestamp, so reading the timestamps alone would take a backup the
// tool has already said not to trust as the evidence the gate ages. Skipping
// rather than refusing the whole response is deliberate: one failed backup
// must not blind the gate to the good ones behind it, and if every full is
// errored the parse fails with that said plainly. Older pgBackRest builds
// omit the field; an absent flag is not a claim either way and is accepted.
func ParseLatestFullBackupInfo(raw []byte, stanza string) (time.Time, error) {
	if len(raw) == 0 || len(raw) > 8<<20 || stanza == "" {
		return time.Time{}, errors.New("invalid pgBackRest info response")
	}
	var response []struct {
		Name   string `json:"name"`
		Backup []struct {
			Type      string `json:"type"`
			Error     *bool  `json:"error"`
			Timestamp struct {
				Stop json.Number `json:"stop"`
			} `json:"timestamp"`
		} `json:"backup"`
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	if err := decoder.Decode(&response); err != nil || len(response) != 1 || response[0].Name != stanza {
		return time.Time{}, errors.New("pgBackRest info response has unexpected stanza or shape")
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return time.Time{}, errors.New("pgBackRest info response has trailing JSON")
	} else if err == nil {
		return time.Time{}, errors.New("pgBackRest info response has trailing JSON")
	}
	var latest time.Time
	var errored bool
	for _, backup := range response[0].Backup {
		if backup.Type != "full" {
			continue
		}
		if backup.Error != nil && *backup.Error {
			errored = true
			continue
		}
		if backup.Timestamp.Stop == "" {
			return time.Time{}, errors.New("pgBackRest full backup has no completed stop timestamp")
		}
		seconds, err := backup.Timestamp.Stop.Int64()
		if err != nil || seconds <= 0 {
			return time.Time{}, errors.New("pgBackRest full backup timestamp is invalid")
		}
		stopped := time.Unix(seconds, 0).UTC()
		if stopped.After(latest) {
			latest = stopped
		}
	}
	if latest.IsZero() {
		if errored {
			return time.Time{}, errors.New("pgBackRest reports no full backup that completed without errors")
		}
		return time.Time{}, errors.New("pgBackRest reports no completed full backup")
	}
	return latest, nil
}
