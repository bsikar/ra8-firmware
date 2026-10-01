//go:build integration

package store

import (
	"context"
	"errors"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// The constructor that decides whether a store can serve encrypted Terraform
// state at all.
//
// It is the only place the 256-bit state key is accepted, and the key never
// reaches PostgreSQL, so nothing downstream can re-check it: a store built
// with the wrong key material is wrong for its whole life. The two halves
// worth holding are that a key of the wrong size is refused before a
// connection is spent, and that the resulting store is actually wired for
// state, which is only visible by asking it for some.

// runtimeDSN is the integration DSN rewritten to the unprivileged runtime
// role. Open refuses the owner role outright (runtimePrivilegeRules), so the
// owner DSN the tests are handed cannot be passed to it, and integrationStore
// builds its Store directly rather than through Open. This is what lets a test
// exercise the real constructor.
//
// Call integrationStore first: it is what creates the role and applies the
// migrations this DSN depends on.
func runtimeDSN(t *testing.T) string {
	t.Helper()
	dsn := os.Getenv("RA8CI_TEST_PG_DSN")
	if dsn == "" {
		t.Fatal("RA8CI_TEST_PG_DSN is required for integration tests")
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		t.Fatalf("parse dsn: %v", err)
	}
	host := config.ConnConfig.Host
	port := config.ConnConfig.Port
	database := config.ConnConfig.Database
	return "postgres://ra8ci_runtime_test:ra8ci_runtime_test_only@" +
		host + ":" + strconv.Itoa(int(port)) + "/" + database + "?sslmode=disable"
}

func TestIntegrationAStateKeyIsRefusedBeforeAConnectionIsSpent(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// A DSN that cannot answer. If the key were judged after the connection,
	// every case below would fail with a dial error instead of naming the
	// key, and an operator holding a 31-byte secret would be told the
	// database is down.
	unusable := "postgres://nobody:nobody@127.0.0.1:1/nowhere?sslmode=disable&connect_timeout=1"

	for _, c := range []struct {
		name string
		key  []byte
	}{
		{"no key", nil},
		{"empty key", []byte{}},
		{"128-bit key", make([]byte, 16)},
		{"192-bit key", make([]byte, 24)},
		{"one byte short", make([]byte, 31)},
		{"one byte long", make([]byte, 33)},
		{"512-bit key", make([]byte, 64)},
	} {
		t.Run(c.name, func(t *testing.T) {
			s, err := OpenWithTerraformStateKey(ctx, unusable, c.key)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("answered %v, want ErrInvalid naming the key", err)
			}
			if s != nil {
				s.Close()
				t.Fatal("a refused open still handed back a store")
			}
			if !strings.Contains(err.Error(), "32 bytes") {
				t.Fatalf("refusal %q does not say what the key must be", err)
			}
		})
	}
}

func TestIntegrationAGoodKeyStillSurfacesTheDatabaseFailure(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// The mirror of the test above: with an acceptable key the DSN is
	// reached, so the caller is told what actually went wrong rather than
	// being handed a key complaint for a database problem.
	s, err := OpenWithTerraformStateKey(ctx,
		"postgres://nobody:nobody@127.0.0.1:1/nowhere?sslmode=disable&connect_timeout=1",
		make([]byte, 32))
	if err == nil {
		s.Close()
		t.Fatal("an unusable database opened")
	}
	if errors.Is(err, ErrInvalid) && strings.Contains(err.Error(), "32 bytes") {
		t.Fatalf("a database failure was reported as a key refusal: %v", err)
	}
	if s != nil {
		s.Close()
		t.Fatal("a failed open still handed back a store")
	}
}

func TestIntegrationOnlyAKeyedStoreWillServeTerraformState(t *testing.T) {
	// integrationStore creates the runtime role and applies the migrations
	// the DSN below depends on.
	integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	dsn := runtimeDSN(t)
	reservation := mustID(t)

	// Opened WITHOUT a key: the store works, but state is not configured, and
	// it says so rather than serving something it cannot decrypt.
	plain, err := Open(ctx, dsn)
	if err != nil {
		t.Fatalf("open without a key: %v", err)
	}
	defer plain.Close()
	if _, _, err := plain.ReadRunnerVMTerraformState(ctx, reservation); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("an unkeyed store answered %v, want ErrUnavailable", err)
	}

	// Opened WITH one: the same call gets past that check. Whether the
	// reservation exists is a different question and not this one, so the
	// assertion is only that state is no longer unconfigured.
	keyed, err := OpenWithTerraformStateKey(ctx, dsn, make([]byte, 32))
	if err != nil {
		t.Fatalf("open with a key: %v", err)
	}
	defer keyed.Close()
	if _, _, err := keyed.ReadRunnerVMTerraformState(ctx, reservation); errors.Is(err, ErrUnavailable) {
		t.Fatalf("a keyed store still reports state unconfigured: %v", err)
	}

	// And the keyed store is a whole store, not a state-only one.
	if err := keyed.Health(ctx); err != nil {
		t.Fatalf("keyed store is not healthy: %v", err)
	}
}

func TestIntegrationAKeyedStoreSealsAndOpensItsOwnState(t *testing.T) {
	integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	dsn := runtimeDSN(t)

	key := make([]byte, 32)
	for i := range key {
		key[i] = byte(i + 1)
	}
	keyed, err := OpenWithTerraformStateKey(ctx, dsn, key)
	if err != nil {
		t.Fatalf("open with a key: %v", err)
	}
	defer keyed.Close()

	reservation := mustID(t)
	plaintext := []byte(`{"version":4,"terraform_version":"1.9.0","serial":1,"lineage":"abc"}`)
	sealed, err := keyed.sealTerraformState(reservation, plaintext)
	if err != nil {
		t.Fatalf("seal: %v", err)
	}
	if strings.Contains(string(sealed), "terraform_version") {
		t.Fatal("sealed state still carries its plaintext")
	}
	opened, err := keyed.openTerraformState(reservation, sealed)
	if err != nil {
		t.Fatalf("open sealed state: %v", err)
	}
	if string(opened) != string(plaintext) {
		t.Fatalf("round trip returned %q, want %q", opened, plaintext)
	}

	// The reservation is bound into the sealed bytes, so state lifted from
	// one runner cannot be read back under another.
	if _, err := keyed.openTerraformState(mustID(t), sealed); err == nil {
		t.Fatal("state sealed for one reservation opened under another")
	}

	// And a store holding a different key cannot read it either, which is
	// what makes the key the thing that matters rather than the database.
	other := make([]byte, 32)
	for i := range other {
		other[i] = byte(i + 2)
	}
	stranger, err := OpenWithTerraformStateKey(ctx, dsn, other)
	if err != nil {
		t.Fatalf("open with another key: %v", err)
	}
	defer stranger.Close()
	if _, err := stranger.openTerraformState(reservation, sealed); err == nil {
		t.Fatal("state sealed under one key opened under another")
	}
}
