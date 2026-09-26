package board

import (
	"testing"
	"time"
)

// A retained lease may carry a beat only from inside its own authority: at or
// after the grant, and strictly before expiry. The before-grant end was
// already pinned; this is the other one.
func TestRetainedHeartbeatAtOrAfterExpiryIsRefused(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	expiry := s.Lease.ExpiresAt
	for name, beat := range map[string]time.Time{
		"at expiry":           expiry,
		"one nanosecond past": expiry.Add(time.Nanosecond),
		"an hour past":        expiry.Add(time.Hour),
	} {
		tampered := s
		lease := *s.Lease
		lease.LastHeartbeatAt = beat
		tampered.Lease = &lease
		if err := Validate(tampered); err == nil {
			t.Fatalf("%s: a beat from outside the lease was accepted", name)
		}
	}
}

func TestRetainedHeartbeatInsideTheLeaseIsAccepted(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	for name, beat := range map[string]time.Time{
		"at the grant":       s.Lease.GrantedAt,
		"midway":             s.Lease.GrantedAt.Add(30 * time.Minute),
		"just before expiry": s.Lease.ExpiresAt.Add(-time.Nanosecond),
		"none reported yet":  {},
	} {
		held := s
		lease := *s.Lease
		lease.LastHeartbeatAt = beat
		held.Lease = &lease
		if err := Validate(held); err != nil {
			t.Fatalf("%s: a beat from inside the lease was refused: %v", name, err)
		}
	}
}

// The reducer cannot produce the refused shape: current() stops a beat at or
// after expiry before holderHeartbeat records anything.
func TestHeartbeatAtExpiryIsRefusedBeforeItIsRecorded(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	_, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, s.Lease.ExpiresAt)
	if err == nil {
		t.Fatal("a beat at expiry was recorded")
	}
	if !s.Lease.LastHeartbeatAt.IsZero() {
		t.Fatalf("the refused beat reached the lease: %v", s.Lease.LastHeartbeatAt)
	}
}

// An extension moves expiry later, never earlier, so a beat recorded under the
// old deadline stays inside the new one.
func TestExtensionKeepsAnEarlierBeatInsideTheLease(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, 10*time.Minute)
	beat := start.Add(time.Minute)
	s, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, beat)
	if err != nil {
		t.Fatalf("heartbeat refused: %v", err)
	}
	s, _, err = Apply(s, Extend{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation,
		Reason: "safe wrap-up", NewExpiry: s.Lease.ExpiresAt.Add(5 * time.Minute)}, beat.Add(time.Minute))
	if err != nil {
		t.Fatalf("extend refused: %v", err)
	}
	if err := Validate(s); err != nil {
		t.Fatalf("extended lease refused: %v", err)
	}
	if !s.Lease.LastHeartbeatAt.Equal(beat) {
		t.Fatalf("extension moved the beat: %v", s.Lease.LastHeartbeatAt)
	}
}

// The refusal is about the beat, not about the phase: a board kept as recovery
// evidence is still held to it, which is exactly where a bad row would sit.
func TestRecoveryEvidenceIsHeldToTheSameHeartbeatWindow(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Minute)
	expired, _, err := Apply(s, Tick{Actor: "server"}, s.Lease.ExpiresAt.Add(time.Second))
	if err != nil {
		t.Fatalf("expiry refused: %v", err)
	}
	if expired.Phase != RecoveryRequired {
		t.Fatalf("board did not fall to recovery: %v", expired.Phase)
	}
	lease := *expired.Lease
	lease.LastHeartbeatAt = lease.ExpiresAt.Add(time.Second)
	expired.Lease = &lease
	if err := Validate(expired); err == nil {
		t.Fatal("recovery evidence carrying a post-expiry beat was accepted")
	}
}
