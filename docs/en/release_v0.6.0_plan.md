# v0.6.0 release plan — pre-tag / post-tag / community split (local-only)

Release-candidate planning doc for PM/HUMAN review. Nothing here has been
pushed/tagged/merged. `duckdb/community-extensions#1980` is untouched.

Proposed tag: **`v0.6.0`** (current published latest is `v0.5.7`).

## A. Before the tag (must land on the branch first)

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

Pre-tag, `README.md` and the roadmap / test_report wording are left as-is.
They still describe the currently published `v0.5.7` and the v0.6 work as
"development branch / not yet released", which is the honest pre-tag state.
Every README badge / "Release:" / "Today (vX)" / "not yet released" flip is
deferred to section B (post-tag) — doing it before the tag exists would
claim a release that is not published.

Decision needed from PM/HUMAN before tagging:
1. Tag number `v0.6.0` approved.
2. Human final test of the full v0.6 set signed off.
3. `#1980` handling (see section C).

## B. After the tag is published (only once `v0.6.0` exists on GitHub)

- Flip "development branch / not yet released" wording to "released in
  v0.6.0" across README, roadmap PT/EN, test_report. (Doing this before the
  tag exists would be a false claim.)
- README badge `v0.5.7` → `v0.6.0` and "Release: **v0.5.7**." →
  "**v0.6.0**" (the badge links to the published release tag, so it only
  becomes valid once `v0.6.0` is published).
- README Current Status: move the v0.6 dev-diagnostics rows into the main
  Done table referencing v0.6.0.
- Roadmap compatibility matrix "Today (v0.5.7)" → "Today (v0.6.0)".
- `release-assets.yml` produces the Linux `.tar.gz` + Windows `.zip` on the
  `v0.6.0` tag push; confirm assets attached to the GitHub Release.
- Paste `docs/en/release_notes_v0.6.0.md` content into the GitHub Release
  body.

## C. community-extensions/description.yml (separate submission step)

`community-extensions/description.yml` is the file copied into a fork of
`duckdb/community-extensions` to drive the centralized build. The community
CI checks out exactly `repo.ref`, so it must point at a **published** tag.

- Prepared in this RC: `version: 0.6.0`, `ref: v0.6.0` (local only).
- **Do not** open / update PR #1980 until: `v0.6.0` is tagged and pushed,
  the GitHub CI (build-linux, fb-matrix) is green on the real runners, and
  PM/HUMAN explicitly authorizes touching #1980.
- README "Community Catalog" line `repo.ref: v0.5.6` → `v0.6.0` belongs to
  this step (it documents the descriptor target), but only flip it once the
  descriptor is actually re-pointed and the tag exists.

## Verification done in this RC pass

- `git diff --check`: clean.
- Old-reference sweep (`v0.5.6`, `v0.5.7`, "development branch", "not yet
  released") performed; see the DEV report for the disposition of each.
- No build/test re-run required: this pass is docs + version metadata only.
  The build/test/sim results from the prior passes (Windows green, Docker
  gcc green, smoke green, 13/13 firebird tests) still hold — no source
  changed.

## Hard boundary

No push, PR, merge, tag, or release. `#1980` untouched. All artifacts local
and for review only.
