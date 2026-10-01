//go:build integration

package store

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// certified plants one principal and one grant, which is the pair every
// client-surface authorization is decided on.
type certified struct {
	store      *Store
	pool       *pgxpool.Pool
	cert       []byte
	principal  string
	repository string
}

func certifiedCaller(t *testing.T, kind, role string) certified {
	t.Helper()
	st, pool := integrationStore(t)
	ctx := context.Background()
	cert := []byte("authorization-integration-" + mustID(t))
	fingerprint := sha256.Sum256(cert)
	principal := "authorization-principal-" + mustID(t)
	repository := "bsikar/ra8ci-authorization-test-" + mustID(t)
	if _, err := pool.Exec(ctx, `INSERT INTO api_principals
		(cert_sha256,principal_id,kind,expires_at)
		VALUES ($1,$2,$3,clock_timestamp()+interval '1 hour')`,
		hex.EncodeToString(fingerprint[:]), principal, kind); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO api_grants(principal_id,repository,role)
		VALUES ($1,$2,$3)`, principal, repository, role); err != nil {
		t.Fatal(err)
	}
	return certified{store: st, pool: pool, cert: cert, principal: principal, repository: repository}
}

// A certificate is authorized on the pair the rule names, and the principal
// it returns is the one the audit trail will attribute the act to.
func TestIntegrationAuthorizeCertificateAdmitsThePairTheRuleNames(t *testing.T) {
	for _, permission := range []string{PermissionRead, PermissionSubmit, PermissionTerraformState} {
		kinds := PrincipalKindsForPermission(permission)
		roles := GrantRolesForPermission(permission)
		if len(kinds) == 0 || len(roles) == 0 {
			t.Fatalf("%s names no kinds or roles", permission)
		}
		c := certifiedCaller(t, kinds[0], roles[0])
		got, err := c.store.AuthorizeCertificate(context.Background(), c.cert, c.repository, permission)
		if err != nil {
			t.Fatalf("%s was refused the pair its own rule names (%s/%s): %v",
				permission, kinds[0], roles[0], err)
		}
		if got != c.principal {
			t.Fatalf("%s authorized as %q, want %q", permission, got, c.principal)
		}
	}
}

// Every way a request can be incoherent is refused as invalid, before any
// certificate is looked up.
func TestIntegrationAuthorizeCertificateRefusesAnIncoherentRequest(t *testing.T) {
	c := certifiedCaller(t, PrincipalKindsForPermission(PermissionRead)[0],
		GrantRolesForPermission(PermissionRead)[0])
	ctx := context.Background()
	for _, bad := range []struct {
		name       string
		cert       []byte
		repository string
		permission string
	}{
		{"no certificate", nil, c.repository, PermissionRead},
		{"an empty certificate", []byte{}, c.repository, PermissionRead},
		{"no repository", c.cert, "", PermissionRead},
		{"a permission that is not one of ours", c.cert, c.repository, "administer"},
		{"an empty permission", c.cert, c.repository, ""},
	} {
		if _, err := c.store.AuthorizeCertificate(ctx, bad.cert, bad.repository, bad.permission); !errors.Is(err, ErrInvalid) {
			t.Fatalf("%s was not refused as invalid: %v", bad.name, err)
		}
	}
}

// The grant is judged on the certificate, the repository and the clock
// together, so each one alone is enough to deny.
func TestIntegrationAuthorizeCertificateDeniesOnEachHalfOfTheGrant(t *testing.T) {
	ctx := context.Background()
	kind := PrincipalKindsForPermission(PermissionRead)[0]
	role := GrantRolesForPermission(PermissionRead)[0]

	t.Run("a certificate the plane has never seen", func(t *testing.T) {
		c := certifiedCaller(t, kind, role)
		if _, err := c.store.AuthorizeCertificate(ctx, []byte("never issued"), c.repository, PermissionRead); !errors.Is(err, ErrDenied) {
			t.Fatalf("an unknown certificate was not denied: %v", err)
		}
	})
	t.Run("a repository the grant does not cover", func(t *testing.T) {
		c := certifiedCaller(t, kind, role)
		if _, err := c.store.AuthorizeCertificate(ctx, c.cert, c.repository+"-other", PermissionRead); !errors.Is(err, ErrDenied) {
			t.Fatalf("a foreign repository was not denied: %v", err)
		}
	})
	t.Run("a principal that has been revoked", func(t *testing.T) {
		c := certifiedCaller(t, kind, role)
		if _, err := c.pool.Exec(ctx,
			`UPDATE api_principals SET revoked_at=clock_timestamp() WHERE principal_id=$1`,
			c.principal); err != nil {
			t.Fatal(err)
		}
		if _, err := c.store.AuthorizeCertificate(ctx, c.cert, c.repository, PermissionRead); !errors.Is(err, ErrDenied) {
			t.Fatalf("a revoked principal was not denied: %v", err)
		}
	})
	t.Run("a principal whose certificate has expired", func(t *testing.T) {
		c := certifiedCaller(t, kind, role)
		if _, err := c.pool.Exec(ctx,
			`UPDATE api_principals SET expires_at=clock_timestamp()-interval '1 second' WHERE principal_id=$1`,
			c.principal); err != nil {
			t.Fatal(err)
		}
		if _, err := c.store.AuthorizeCertificate(ctx, c.cert, c.repository, PermissionRead); !errors.Is(err, ErrDenied) {
			t.Fatalf("an expired principal was not denied: %v", err)
		}
	})
}

// A grant that is real but names a pair this permission does not admit is
// denied, which is the rule doing the work rather than the row's existence.
func TestIntegrationAuthorizeCertificateDeniesAGrantTheRuleDoesNotAdmit(t *testing.T) {
	ctx := context.Background()
	// A pair that is valid for some permission but refused for this one.
	var kind, role, permission string
	for _, candidate := range []string{PermissionRead, PermissionSubmit, PermissionTerraformState} {
		for _, otherKind := range PrincipalKindsForPermission(PermissionTerraformState) {
			for _, otherRole := range GrantRolesForPermission(PermissionTerraformState) {
				if !ClientAPIGrantAllowed(otherKind, otherRole, candidate) {
					kind, role, permission = otherKind, otherRole, candidate
				}
			}
		}
	}
	if permission == "" {
		t.Skip("every kind/role pair is admitted by every permission")
	}
	c := certifiedCaller(t, kind, role)
	if _, err := c.store.AuthorizeCertificate(ctx, c.cert, c.repository, permission); !errors.Is(err, ErrDenied) {
		t.Fatalf("%s admitted %s/%s, which its rule refuses: %v", permission, kind, role, err)
	}
}

// The repository behind a run is what a later authorization is decided
// against, so an absent or malformed run is reported rather than guessed.
func TestIntegrationLookupRunRepositoryAnswersOnlyForARunOnFile(t *testing.T) {
	st, _, _, _, run, _ := dispatchFixture(t)
	ctx := context.Background()

	got, err := st.LookupRunRepository(ctx, run.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got != run.Repository {
		t.Fatalf("run %s reported repository %q, want %q", run.ID, got, run.Repository)
	}
	for _, bad := range []string{"", "not-a-uuid", strings.Repeat("a", 40)} {
		if _, err := st.LookupRunRepository(ctx, bad); !errors.Is(err, ErrInvalid) {
			t.Fatalf("run ID %q was not refused as invalid: %v", bad, err)
		}
	}
	absent, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := st.LookupRunRepository(ctx, absent); !errors.Is(err, ErrNotFound) {
		t.Fatalf("an absent run was not reported as not found: %v", err)
	}
}

// A denial is committed before the response goes out, so the record of a
// refused request survives whatever the HTTP layer then does.
func TestIntegrationAuditDeniedCommitsTheRefusal(t *testing.T) {
	st, pool := integrationStore(t)
	ctx := context.Background()
	actor := "denied-actor-" + mustID(t)
	target := "denied-target-" + mustID(t)

	if err := st.AuditDenied(ctx, actor, "run.read", target); err != nil {
		t.Fatal(err)
	}
	var targetType, outcome string
	var reason json.RawMessage
	var previous, next *string
	if err := pool.QueryRow(ctx,
		`SELECT target_type, outcome, previous_state, new_state, reason FROM audit
		 WHERE actor_id=$1 AND target_id=$2`, actor, target).
		Scan(&targetType, &outcome, &previous, &next, &reason); err != nil {
		t.Fatalf("read denial audit: %v", err)
	}
	if targetType != "api" || outcome != "denied" {
		t.Fatalf("the denial filed target_type=%q outcome=%q, want api/denied", targetType, outcome)
	}
	// A denial moved nothing, so it carries no before or after state.
	if previous != nil || next != nil {
		t.Fatalf("the denial claimed a state change: previous=%v next=%v", previous, next)
	}
	if string(reason) != "{}" {
		t.Fatalf("the denial filed reason %s, want an empty object", reason)
	}

	for _, bad := range []struct{ actor, action, target string }{
		{"", "run.read", target},
		{actor, "", target},
		{actor, "run.read", ""},
	} {
		if err := st.AuditDenied(ctx, bad.actor, bad.action, bad.target); !errors.Is(err, ErrInvalid) {
			t.Fatalf("an incomplete denial was accepted: %+v, %v", bad, err)
		}
	}
}

// WaitForReady answers as soon as the store is healthy, and hands back the
// context's own error rather than a wrapped one when it is not.
func TestIntegrationWaitForReadyAnswersOnHealthAndOnCancellation(t *testing.T) {
	st, _ := integrationStore(t)
	ready, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := st.WaitForReady(ready, 10*time.Millisecond); err != nil {
		t.Fatalf("a healthy store was not reported ready: %v", err)
	}
	if err := st.WaitForReady(ready, 0); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a zero retry interval was accepted: %v", err)
	}

	closed, _ := integrationStore(t)
	closed.Close()
	short, stop := context.WithTimeout(context.Background(), 150*time.Millisecond)
	defer stop()
	err := closed.WaitForReady(short, 10*time.Millisecond)
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("waiting on a closed store ended with %v, want the context deadline", err)
	}
}
