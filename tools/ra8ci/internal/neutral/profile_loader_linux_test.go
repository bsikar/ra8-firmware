// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestNewLinuxObserverFromFileBindsExactReviewedProfile(t *testing.T) {
	profile := validProfileFixture()
	raw, err := json.Marshal(profile)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "neutral-profile.json")
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	observer, err := NewLinuxObserverFromFile(path, LinuxObserverConfig{
		Gate: &testHardwareGate{}, Inspector: testActivityInspector{},
		Readers:       map[string]SignalReader{"tapo": testSignalReader{}},
		ProfileSHA256: strings.Repeat("f", 64),
	})
	if err != nil {
		t.Fatalf("load observer from reviewed profile: %v", err)
	}
	digest := sha256.Sum256(raw)
	if observer.digest != hex.EncodeToString(digest[:]) || observer.profile.BoardID != profile.BoardID {
		t.Fatalf("observer profile binding digest=%s board=%s", observer.digest, observer.profile.BoardID)
	}
}
