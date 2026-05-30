# Documentation parity map

Docs must stay standardized across Portuguese and English.

Rule:

- what exists in Portuguese should exist in English
- what exists in English should exist in Portuguese
- structure, meaning, status, caveats, and roadmap decisions should match
- if a document intentionally exists in one language only, this file must
  say why

## Public docs

| Topic | English | Portuguese | Status |
|---|---|---|---|
| Usage guide | `docs/en/usage_guide.md` | `docs/pt/usage_guide.md` | Paired |
| Function manual | `docs/en/function_manual.md` | `docs/pt/function_manual.md` | Paired |
| Roadmap | `docs/en/roadmap.md` | `docs/pt/roadmap.md` | Paired for current strategy; EN keeps full historical detail |
| Linux guide | `docs/en/guide_linux.md` | pending | Needs PT counterpart before major doc release |
| Windows guide | `docs/en/guide_windows.md` | pending | Needs PT counterpart before major doc release |
| Observability | `docs/en/observability.md` | covered inside `docs/pt/function_manual.md` | Acceptable for now; split PT page when observability changes again |
| Architecture | `docs/en/architecture.md` | pending | Internal/developer doc; translate when DEV workflow depends on it |
| Test report | `docs/en/test_report.md` | pending | Release evidence; translate or summarize per release |
| v0.4 review report | `docs/en/review_report_v0.4.md` | pending | Historical report; translate only if referenced publicly |

## Historical closeouts

These are local project-management records written in Portuguese:

- `docs/pt/phase1_closeout.md`
- `docs/pt/phase2_closeout.md`
- `docs/pt/phase3_closeout.md`
- `docs/pt/hotfix_v0.5.5_closeout.md`
- `docs/pt/hotfix_v0.5.6_closeout.md`

They are not user-facing manuals. English counterparts are optional unless
they become public release notes, onboarding material, or active DEV input.

## Required review before closing doc tasks

Before closing any documentation task:

1. Check whether the changed topic has both PT and EN surfaces.
2. Update both surfaces when both exist.
3. If only one language is updated, record the reason in the final report.
4. Keep examples and status labels aligned.
5. Keep roadmap decisions aligned in `docs/en/roadmap.md` and
   `docs/pt/roadmap.md`.
