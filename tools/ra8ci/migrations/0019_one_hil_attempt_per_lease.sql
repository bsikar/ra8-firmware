-- A physical board lease can execute only one live HIL task at a time.
CREATE UNIQUE INDEX task_attempts_one_active_hil_per_lease
    ON task_attempts (board_lease_id)
    WHERE board_lease_id IS NOT NULL AND state IN ('issued','acknowledged','running');
