-- A just-in-time runner credential is single use and is minted before the
-- guest that consumes it exists. Nothing in the forge ever says "that
-- credential was wasted": a scale-set listener got that for free because it
-- held the assignment itself. This plane has to state the deadline, so every
-- reservation carries one explicitly and the reaper reads it rather than
-- guessing an age.
ALTER TABLE runner_vms ADD COLUMN unclaimed_deadline timestamptz;
UPDATE runner_vms SET unclaimed_deadline = created_at + interval '30 minutes'
    WHERE unclaimed_deadline IS NULL;
ALTER TABLE runner_vms ALTER COLUMN unclaimed_deadline SET NOT NULL;
ALTER TABLE runner_vms ADD CONSTRAINT runner_vms_unclaimed_deadline_future
    CHECK (unclaimed_deadline > created_at);

-- claimed_at is the moment a job actually took the runner. It is set once and
-- never cleared: a reservation that has carried a job is out of the reaper's
-- reach for good, whatever happens to that job afterwards.
ALTER TABLE runner_vms ADD COLUMN claimed_at timestamptz;
ALTER TABLE runner_vms ADD CONSTRAINT runner_vms_claimed_after_creation
    CHECK (claimed_at IS NULL OR claimed_at >= created_at);

-- The reaper's candidate set, in the order it wants them: soonest deadline
-- first. A released reservation has nothing left to revoke.
CREATE INDEX runner_vms_unclaimed_idx ON runner_vms (unclaimed_deadline)
    WHERE claimed_at IS NULL AND state <> 'released';
