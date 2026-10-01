// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writePlanFile(t *testing.T, contents string) (string, string) {
	t.Helper()
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatalf("protect plan directory: %v", err)
	}
	planFile := filepath.Join(directory, "saved.tfplan")
	if err := os.WriteFile(planFile, []byte(contents), 0o600); err != nil {
		t.Fatalf("write plan file: %v", err)
	}
	sum := sha256.Sum256([]byte(contents))
	return planFile, hex.EncodeToString(sum[:])
}

func TestApprovedPlanDigestAcceptsTheRecordedPlan(t *testing.T) {
	planFile, digest := writePlanFile(t, "approved plan bytes")
	if err := requireApprovedPlan(planFile, digest); err != nil {
		t.Fatalf("recorded plan must be accepted: %v", err)
	}
}

func TestSubstitutedPlanIsRefusedAgainstTheApprovedIntent(t *testing.T) {
	planFile, digest := writePlanFile(t, "approved plan bytes")
	if err := os.WriteFile(planFile, []byte("a different plan entirely"), 0o600); err != nil {
		t.Fatalf("substitute plan file: %v", err)
	}
	err := requireApprovedPlan(planFile, digest)
	if err == nil {
		t.Fatal("a plan that no longer matches the recorded digest must be refused")
	}
	if !strings.Contains(err.Error(), "approved apply intent") {
		t.Fatalf("refusal must name the apply intent, got %v", err)
	}
}

func TestSingleByteChangeToThePlanIsRefused(t *testing.T) {
	planFile, digest := writePlanFile(t, "approved plan bytes")
	if err := os.WriteFile(planFile, []byte("approved plan byteS"), 0o600); err != nil {
		t.Fatalf("substitute plan file: %v", err)
	}
	if requireApprovedPlan(planFile, digest) == nil {
		t.Fatal("a one byte change to the saved plan must be refused")
	}
}

func TestEmptyApprovedDigestIsRefusedRatherThanSkipped(t *testing.T) {
	planFile, _ := writePlanFile(t, "approved plan bytes")
	err := requireApprovedPlan(planFile, "")
	if err == nil {
		t.Fatal("an absent digest must refuse the apply, never wave it through")
	}
	if !strings.Contains(err.Error(), "recorded plan digest") {
		t.Fatalf("refusal must name the missing digest, got %v", err)
	}
}

func TestMalformedApprovedDigestIsRefused(t *testing.T) {
	planFile, digest := writePlanFile(t, "approved plan bytes")
	for _, malformed := range []string{
		strings.ToUpper(digest),
		digest[:63],
		digest + "0",
		strings.Repeat("z", 64),
	} {
		if requireApprovedPlan(planFile, malformed) == nil {
			t.Fatalf("digest %q is not a lowercase sha256 and must be refused", malformed)
		}
	}
}

func TestMissingPlanFileIsRefusedBeforeAnyComparison(t *testing.T) {
	planFile, digest := writePlanFile(t, "approved plan bytes")
	if err := os.Remove(planFile); err != nil {
		t.Fatalf("remove plan file: %v", err)
	}
	err := requireApprovedPlan(planFile, digest)
	if err == nil {
		t.Fatal("a plan file that is gone must be refused")
	}
	if !strings.Contains(err.Error(), "read Terraform saved plan digest") {
		t.Fatalf("refusal must name the unreadable plan, got %v", err)
	}
}

func TestEmptyPlanFileIsRefused(t *testing.T) {
	planFile, digest := writePlanFile(t, "approved plan bytes")
	if err := os.WriteFile(planFile, nil, 0o600); err != nil {
		t.Fatalf("truncate plan file: %v", err)
	}
	if requireApprovedPlan(planFile, digest) == nil {
		t.Fatal("an emptied plan file must be refused")
	}
}

func TestApplyStillRefusesAPlanOutsideTheWorkspaceFirst(t *testing.T) {
	planFile, digest := writePlanFile(t, "approved plan bytes")
	session := &TerraformSession{
		runtime:       &TerraformRuntime{config: TerraformConfig{}},
		reservationID: "0199f3a1-2b4c-7d8e-9f01-23456789abcd",
		workspace:     t.TempDir(),
		environment:   []string{"PATH=/usr/bin:/bin"},
	}
	err := session.Apply(t.Context(), planFile, digest)
	if err == nil {
		t.Fatal("a plan outside the reservation workspace must still be refused")
	}
	if !strings.Contains(err.Error(), "inside its reservation workspace") {
		t.Fatalf("the workspace fence must keep naming its own reason, got %v", err)
	}
}
