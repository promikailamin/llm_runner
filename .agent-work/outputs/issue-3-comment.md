Implemented issue #3.

Changes:
- The `tunnel` release is no longer deleted when the workflow ends, times out, or is cancelled. It is kept and its status is changed to `offline`.
- The release description is now a single fully valid JSON document with `status=online` while serving and `status=offline`, `ended_at`, and `offline_reason` afterward.
- `publish-tunnel-release.sh` now creates the release once and updates the same release on later runs instead of deleting and recreating it.
- Added `scripts/mark-tunnel-release-offline.sh`.
- Updated `llm.yml`, `tunnel-cleanup.yml`, `cleanup-stale-tunnel-release.sh`, README, and tests.
- `delete-tunnel-release.sh` remains available for explicit manual deletion but is no longer called by the workflows.

Validation:
- `python3 -m unittest discover -s .agent-work/tests -p 'test_*.py' -v`: 17 tests passed.
- `shellcheck -x scripts/*.sh`: passed.
- `actionlint` 1.7.12: passed.
- Added a fake-`gh` test covering JSON publish, in-place republish, offline marking, and verification that no release deletion happens.

Commit: `f689075` (`Keep tunnel release and mark it offline with JSON`), pushed to `main`.

Remaining: no new GitHub Actions end-to-end run was executed from this job; validation was static plus simulated release operations.
