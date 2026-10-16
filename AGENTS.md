# spice2x fork workspace instructions

## Project shorthand

- `%pjsync`: fetch the latest published upstream release, including prereleases. From `origin/main`, create a `codex/sync-<tag>` branch, merge the release tag, reconcile fork changes, run relevant builds/tests, and open a PR. If already included, do not downgrade.

## Project scope

- Read `README.md` and affected source. Limit changes to the requested task; follow upstream layout and style.
- Edit native and controller source directly. Keep both on `main`; do not introduce a patch stack or release branch.
- Keep the original repository as `upstream` and this fork as `origin`. Push only task branches to `origin`.
- Follow GitHub flow: `codex/` task branch, PR to `main`, required checks and review, then merge. Never bypass protection, force-push, or rewrite published history.
- Preserve existing GHA workflows. Before changing one, present the reason and exact diff and obtain explicit user authorization. Do not introduce a replacement release workflow.
- Deploy Pages through GHA from controller source on `main`; do not commit generated assets.
- Keep fork operation rules only in `AGENTS.md` and `CONTRIBUTING.md` identical to upstream. Put fork usage guides in GitHub Wiki; keep them concise: steps, paths and necessary limitations. Omit implementation details, work history and build/test records.
- Use upstream CI/release scripts and naming; do not add a separate version file or packager. Build task branches from a clean detached checkout when needed. Verify release artifacts match the tested `main` commit; preserve existing tags and assets.
- Put build/test output in ignored `build/` or component output directories; keep private runtime state, diagnostics and review records in ignored `local/`.
- Run affected tests and upstream builds. For controller changes, run `npm ci`, `npm test` and `npm run build` in `controller/` with Node.js 22.12+.
- Preserve API, command-line and config compatibility except the intentionally removed `-resizecfgpath` option. Guard XP-only behavior with `SPICE_XP`; distinguish automated checks from real cabinet/driver/input verification.
