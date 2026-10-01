// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"errors"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
)

// Terraform state is the one thing the control plane stores that a runner
// hands it verbatim, and it carries the addresses, names and identifiers of
// every machine in a lab. It is sealed before it reaches a row and opened
// on the way back out.
//
// The sealing is pure: it needs the key and nothing else, no database. That
// makes the property worth having testable right here, and it is not a
// property about encryption in general but about BINDING. Every reservation
// seals under its own additional data, so a ciphertext lifted from one
// reservation's row and dropped into another's does not open, even though
// the key is the same one. Without that, a runner could be handed another
// runner's state by a row swap.
//
// A Store with a key and no connection is enough to drive all of it: the
// pool is only consulted to decide whether encryption is configured at all.

const (
	reservationOne = "01996f90-3415-7cfe-8ff1-600058131b10"
	reservationTwo = "01996f90-3415-7cfe-8ff1-600058131b11"
)

func sealingStore(t *testing.T) *Store {
	t.Helper()
	key := make([]byte, 32)
	if _, err := rand.Read(key); err != nil {
		t.Fatal(err)
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		t.Fatal(err)
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		t.Fatal(err)
	}
	return &Store{pool: &pgxpool.Pool{}, terraformStateAEAD: aead}
}

// The round trip returns exactly what went in, and what sits in the row is
// not the state itself.
func TestSealedTerraformStateComesBackAsItWentIn(t *testing.T) {
	s := sealingStore(t)
	body := []byte(`{"version":4,"serial":7,"lineage":"3f2b1c4d-1a2b-4c3d-8e9f-0a1b2c3d4e5f"}`)

	sealed, err := s.sealTerraformState(reservationOne, body)
	if err != nil {
		t.Fatalf("seal: %v", err)
	}
	if bytes.Contains(sealed, []byte("lineage")) || bytes.Contains(sealed, body) {
		t.Fatal("the sealed row carries the state in the clear")
	}
	opened, err := s.openTerraformState(reservationOne, sealed)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	if !bytes.Equal(opened, body) {
		t.Fatalf("opened = %q, want the body that was sealed", opened)
	}
}

// The reservation is bound into the ciphertext, so state cannot be moved
// between reservations by moving the row.
func TestStateSealedForOneReservationDoesNotOpenUnderAnother(t *testing.T) {
	s := sealingStore(t)
	sealed, err := s.sealTerraformState(reservationOne, []byte("lab state"))
	if err != nil {
		t.Fatal(err)
	}
	for _, other := range []string{reservationTwo, "", strings.ToUpper(reservationOne), reservationOne + " "} {
		opened, err := s.openTerraformState(other, sealed)
		if err == nil {
			t.Fatalf("state opened under reservation %q: %q", other, opened)
		}
		if !errors.Is(err, ErrUnavailable) || !strings.Contains(err.Error(), "decrypt Terraform state") {
			t.Fatalf("reservation %q: err = %v, want a decrypt refusal", other, err)
		}
	}
}

// A row that was cut short, or edited anywhere along its length, is refused
// rather than half-decrypted.
func TestATamperedOrTruncatedRowIsRefused(t *testing.T) {
	s := sealingStore(t)
	sealed, err := s.sealTerraformState(reservationOne, []byte("lab state"))
	if err != nil {
		t.Fatal(err)
	}

	for name, cut := range map[string][]byte{
		"nothing at all":       nil,
		"empty":                {},
		"a nonce with no body": sealed[:12],
		"one byte short":       sealed[:len(sealed)-1],
		"nonce and tag only":   sealed[:12+16],
		"a single stray byte":  {0x00},
	} {
		if _, err := s.openTerraformState(reservationOne, cut); err == nil {
			t.Fatalf("%s: a short row opened", name)
		} else if !errors.Is(err, ErrUnavailable) {
			t.Fatalf("%s: err = %v, want ErrUnavailable", name, err)
		}
	}

	for index := range sealed {
		edited := append([]byte(nil), sealed...)
		edited[index] ^= 0x01
		if _, err := s.openTerraformState(reservationOne, edited); err == nil {
			t.Fatalf("a row edited at byte %d still opened", index)
		}
	}
}

// Two seals of the same state under the same reservation differ, so a row
// never reveals that the state did not change.
func TestTwoSealsOfTheSameStateAreNotTheSameRow(t *testing.T) {
	s := sealingStore(t)
	body := []byte("lab state")
	seen := make(map[string]bool, 16)
	for i := 0; i < 16; i++ {
		sealed, err := s.sealTerraformState(reservationOne, body)
		if err != nil {
			t.Fatal(err)
		}
		if seen[string(sealed)] {
			t.Fatal("two seals of the same state produced the same row")
		}
		seen[string(sealed)] = true
		opened, err := s.openTerraformState(reservationOne, sealed)
		if err != nil || !bytes.Equal(opened, body) {
			t.Fatalf("seal %d did not round trip: %v", i, err)
		}
	}
}

// An empty state seals and opens like any other. The bounds on what a
// state may contain belong to parseTerraformState, not to the sealing, and
// keeping them apart is what lets either be changed on its own.
func TestSealingHoldsNoOpinionOnWhatTheStateSays(t *testing.T) {
	s := sealingStore(t)
	for name, body := range map[string][]byte{
		"not JSON at all":  []byte("this is not a Terraform state"),
		"a lone NUL byte":  {0x00},
		"a megabyte of it": bytes.Repeat([]byte("a"), 1<<20),
	} {
		sealed, err := s.sealTerraformState(reservationOne, body)
		if err != nil {
			t.Fatalf("%s: seal: %v", name, err)
		}
		opened, err := s.openTerraformState(reservationOne, sealed)
		if err != nil {
			t.Fatalf("%s: open: %v", name, err)
		}
		if !bytes.Equal(opened, body) {
			t.Fatalf("%s: did not round trip", name)
		}
	}
}

// A plane whose state encryption was never configured refuses both
// directions rather than storing a lab's addresses in the clear.
func TestAPlaneWithoutStateEncryptionSealsNothing(t *testing.T) {
	configured := sealingStore(t)
	for name, s := range map[string]*Store{
		"no store at all":    nil,
		"nothing configured": {},
		"a key but no connection": {
			terraformStateAEAD: configured.terraformStateAEAD,
		},
		"a connection but no key": {pool: &pgxpool.Pool{}},
	} {
		if _, err := s.sealTerraformState(reservationOne, []byte("lab state")); !errors.Is(err, ErrUnavailable) {
			t.Fatalf("%s: seal err = %v, want ErrUnavailable", name, err)
		} else if !strings.Contains(err.Error(), "not configured") {
			t.Fatalf("%s: seal err = %v, want it to say encryption is not configured", name, err)
		}
		if _, err := s.openTerraformState(reservationOne, []byte("whatever")); !errors.Is(err, ErrUnavailable) {
			t.Fatalf("%s: open err = %v, want ErrUnavailable", name, err)
		}
	}
}

// An EMPTY state is the one body that seals and does not open again. The
// seal is nonce plus tag and nothing else, and the truncation guard
// requires strictly more than that, so it reads its own output as a cut
// row. Nothing legitimate reaches it: parseTerraformState refuses an empty
// body with ErrInvalid long before a seal is asked for, so the guard is
// right to insist on at least one byte of payload. Pinned as behaviour,
// not fixed.
func TestAnEmptyStateSealsToNothingThatCanBeOpened(t *testing.T) {
	s := sealingStore(t)
	sealed, err := s.sealTerraformState(reservationOne, nil)
	if err != nil {
		t.Fatalf("seal: %v", err)
	}
	if len(sealed) != 12+16 {
		t.Fatalf("an empty state sealed to %d bytes, want nonce plus tag", len(sealed))
	}
	if _, err := s.openTerraformState(reservationOne, sealed); err == nil {
		t.Fatal("an empty state opened")
	} else if !strings.Contains(err.Error(), "truncated") {
		t.Fatalf("err = %v, want the truncation refusal", err)
	}

	if _, _, err := parseTerraformState(nil); !errors.Is(err, ErrInvalid) {
		t.Fatalf("parse of an empty state = %v, want ErrInvalid: nothing may reach the seal empty", err)
	}
	if _, err := s.openTerraformState(reservationOne, sealed[:len(sealed)-1]); err == nil {
		t.Fatal("a row one byte shorter than nonce plus tag opened")
	}
	oneByte, err := s.sealTerraformState(reservationOne, []byte("x"))
	if err != nil {
		t.Fatal(err)
	}
	opened, err := s.openTerraformState(reservationOne, oneByte)
	if err != nil || !bytes.Equal(opened, []byte("x")) {
		t.Fatalf("a single byte of state did not round trip: %v", err)
	}
}
