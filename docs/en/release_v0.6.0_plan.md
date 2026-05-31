# v0.6.0 release plan — execution log and community follow-up

Release execution record for PM/HUMAN review. The project release has been
merged, tagged, and published; `duckdb/community-extensions#1980` remains the
next explicit follow-up.

Published tag: **`v0.6.0`**.

## A. Before the tag (completed)

These describe what the tagged commit will contain. They are code/docs
already committed on `dev/phase4-profile-table` plus the doc/version edits
prepared in this RC pass:

- Phase 4 feature commits (already local):
  `91c7b0f`, `695656f`, `e6e30ce`, `262c443`, `01e491e`, `7e179c4`,
  `a448f4f` (defer), `b662edc` (GCC fix).
- Release notes: `docs/en/release_notes_v0.6.0.md` (this RC).
- `vcpkg.json` `version-string`: `0.5.7` → `0.6.0`.
- `community-extensions/description.yml`: `version`/`ref` bumped to
  `0.6.0` / `v0.6.0` **locally** (proposal only — not submitted; see C).

Pre-tag, `README.md` and the roadmap / test_report wording were left as-is
so they did not claim a release before the tag existed. Those flips are now
completed in section B.

Completed:

1. Tag number `v0.6.0` approved.
2. Human final test of the full v0.6 set signed off.
3. PR #16 merged to `main`.

## B. After the tag is published (completed)

- Flipped "development branch / not yet released" wording to "released in
  v0.6.0" across README, roadmap PT/EN, test_report.
- README badge `v0.5.7` → `v0.6.0` and "Release: **v0.5.7**." →
  "**v0.6.0**" (the badge links to the published release tag, so it only
  becomes valid once `v0.6.0` is published).
- README Current Status: v0.6 diagnostics rows now reference v0.6.0 instead
  of local implementation status.
- Roadmap compatibility matrix "Today (v0.5.7)" → "Today (v0.6.0)".
- `release-assets.yml` produced the Linux `.tar.gz` + Windows `.zip` on the
  `v0.6.0` tag push; assets attached to the GitHub Release.
- `docs/en/release_notes_v0.6.0.md` content pasted into the GitHub Release
  body.

## C. community-extensions/description.yml (separate submission step)

`community-extensions/description.yml` is the file copied into a fork of
`duckdb/community-extensions` to drive the centralized build. The community
CI checks out exactly `repo.ref`, so it must point at a **published** tag.

- Prepared in the v0.6.0 release commit: `version: 0.6.0`, `ref: v0.6.0`.
- Open / update PR #1980 only after the GitHub CI is green on the real
  runners and PM/HUMAN explicitly authorizes touching #1980.
- README "Community Catalog" line `repo.ref: v0.5.6` → `v0.6.0` is complete
  in this repo. Updating `duckdb/community-extensions#1980` remains separate.

## Verification done

- `git diff --check`: clean.
- Old-reference sweep (`v0.5.6`, `v0.5.7`, "development branch", "not yet
  released") performed and then flipped after tag publication where needed.
- PR #16 checks green on GitHub: Linux, Windows, and FB 3/4/5 matrix.
- Release assets workflow green for tag `v0.6.0`.
- Pre-release local validation: Windows green, Docker gcc green, smoke green,
  13/13 firebird tests.

## Remaining boundary

`duckdb/community-extensions#1980` must be updated explicitly as a separate
community submission step. Do not mix post-release documentation cleanup with
the community PR update without PM/HUMAN authorization.
