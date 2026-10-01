// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilpolicy"
)

// Three doors this package keeps that nothing else in the tree keeps for it:
// the parser handed no manifest at all, a wire protocol the runner cannot
// speak, and a Spec that reached Decide without passing through Parse.

// Parse reads whatever a caller hands it, including nothing. Refusing a nil
// reader by name keeps the refusal here rather than letting a nil dereference
// reach the lab runner as a crash.
func TestParsingNothingIsRefusedRatherThanAttempted(t *testing.T) {
	if _, err := Parse(nil, "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf"); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("a nil manifest reader was accepted: %v", err)
	}
	// And the same path with a real reader still parses, so the refusal above
	// is about the absent reader and not about the path.
	if _, err := Parse(strings.NewReader("HIL_MODE=alive\n"),
		"examples/ek_ra8d2/hw_validated/hil/demo/hil.conf"); err != nil {
		t.Fatalf("an ordinary manifest was refused: %v", err)
	}
}

// HIL_PROTO names the socket the ethernet mode opens. Only the two the runner
// can actually open are admitted: anything else is a manifest that would be
// accepted here and fail on the bench, which is the expensive place to find
// out.
func TestAWireProtocolTheRunnerCannotOpenIsRefused(t *testing.T) {
	const path = "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf"
	for _, proto := range []string{"tcp", "udp"} {
		manifest := "HIL_MODE=hil_eth_tcp\nHIL_BOARD_IP=192.168.1.50\nHIL_PORT=5000\nHIL_PROTO=" + proto + "\n"
		if _, err := Parse(strings.NewReader(manifest), path); err != nil {
			t.Fatalf("%s was refused: %v", proto, err)
		}
	}
	for _, proto := range []string{"sctp", "TCP", "tcp6", "icmp", "raw"} {
		manifest := "HIL_MODE=hil_eth_tcp\nHIL_BOARD_IP=192.168.1.50\nHIL_PORT=5000\nHIL_PROTO=" + proto + "\n"
		_, err := Parse(strings.NewReader(manifest), path)
		if !errors.Is(err, ErrInvalidManifest) {
			t.Fatalf("HIL_PROTO=%s was accepted: %v", proto, err)
		}
		if !strings.Contains(err.Error(), "unsupported wire protocol") {
			t.Fatalf("HIL_PROTO=%s was refused without naming the protocol rule: %v", proto, err)
		}
	}
}

// Decide is given a Spec by its caller, and a Spec is an ordinary struct: it
// does not have to have come from Parse. So Decide holds the declared window
// to the estimator's own bounds at its front door, which is what keeps a
// hand-built Spec from deciding a lab timeout from a number no manifest could
// have declared. The estimator's matching refusal is therefore unreachable
// through Decide by construction, and that is the point of this test: the two
// bounds agree, so the refusal always carries ErrInvalidManifest and names the
// manifest rather than the estimator.
func TestADeclaredWindowTheEstimatorRefusesIsNotDecided(t *testing.T) {
	spec, workload, options := decisionFixture(t)
	spec.TimeoutDeclared = true
	for _, seconds := range []int{0, -1, hilpolicy.MaximumSeconds + 1, 1 << 30} {
		spec.TimeoutSeconds = seconds
		if _, err := Decide(context.Background(), spec, workload, nil, options); !errors.Is(err, ErrInvalidManifest) {
			t.Fatalf("a declared window of %ds was accepted: %v", seconds, err)
		}
	}
	// Both ends of the bound still decide, so the refusals above are about
	// being outside it rather than about declaring a window at all.
	for _, seconds := range []int{1, 12} {
		spec.TimeoutSeconds = seconds
		spec.SafetyMaximumSeconds = 0
		decision, err := Decide(context.Background(), spec, workload, nil, options)
		if err != nil || decision.ValidityWindow != time.Duration(seconds)*time.Second || decision.Source != "hil.conf" {
			t.Fatalf("a declared %ds window was not decided: %+v err=%v", seconds, decision, err)
		}
	}
}
