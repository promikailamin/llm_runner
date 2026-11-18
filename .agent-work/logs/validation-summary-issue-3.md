# Validation summary for issue 3

Date: 2026-11-18

## Requested behavior

- Keep the `tunnel` release after the serving workflow ends.
- Change the release status to `offline` instead of deleting the release or tag.
- Store the release description as a single, fully valid JSON document.

## Implementation

- `scripts/publish-tunnel-release.sh` now writes a JSON release body and updates
  an existing `tunnel` release in place instead of deleting and recreating it.
  It uploads `tunnel.json` with `--clobber` so the asset is refreshed.
- `scripts/mark-tunnel-release-offline.sh` reads the existing release body,
  preserves its JSON fields, and sets `status` to `offline` with `ended_at` and
  `offline_reason`.
- `scripts/cleanup-stale-tunnel-release.sh` now marks stale releases offline
  rather than deleting them.
- `.github/workflows/llm.yml` marks the release offline from the keep-alive
  signal trap, the final `serve` step, and the `cleanup` job.
- `.github/workflows/tunnel-cleanup.yml` marks stale releases offline.
- `scripts/delete-tunnel-release.sh` remains available for explicit manual
  deletion but is no longer called by the workflows.
- `README.md` documents the new lifecycle and JSON body.

## Static checks

- `python3 -m unittest discover -s .agent-work/tests -p 'test_*.py' -v`:
  17 tests passed.
- `shellcheck -x scripts/*.sh`: passed.
- `actionlint` 1.7.12 on all workflows: passed.
- `yamllint` only reported the repository's pre-existing `document-start`,
  `truthy`, and line-length warnings.

## Release script test

The new test `test_publish_and_offline_tunnel_release_json` uses a fake `gh`
binary and verifies that:

- the first publish creates a release with valid JSON and `status=online`,
- republishing updates the same release with `gh release edit` and does not
  create a second release or delete one,
- marking offline preserves the address and run fields while changing
  `status` to `offline`, adding `ended_at` and `offline_reason`,
- no `gh release delete` call is made in either path.

Logs:

- `.agent-work/logs/test-issue-3.log`
- `.agent-work/logs/shellcheck-issue-3.log`
- `.agent-work/logs/actionlint-issue-3.log`
- `.agent-work/logs/yamllint-issue-3.log`
