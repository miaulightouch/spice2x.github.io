# spice2x Arcade Room fork workspace instructions

## Project shorthand

- `%pjsync`: fetch the most recent published upstream release (including prereleases), create a `codex/sync-<upstream-tag>` task branch from `origin/main`, merge its release tag, reconcile the fork's source changes, and run the relevant builds and tests. If the release is already included, do not downgrade the source. Update `UPSTREAM_BASE` and `FORK_VERSION` when the upstream base changes. Open a pull request to the fork's `main`; do not push or merge directly into `main`.

## Project scope

- Read `README.md`, `docs/Fork-workflow.md`, and the affected source before changing code.
- Maintain one task-focused spice2x fork. Edit `src/spice2x/` and `controller/` directly, using the upstream layout and surrounding style. Do not introduce a patch stack or a separate release source branch.
- Keep the original spice2x repository as `upstream` and this fork as `origin`. Push only task branches to `origin`; never push to `upstream`.
- Follow GitHub flow: one bounded task per branch, pull request to `main`, required checks, review, then merge. Use `codex/` for agent-created task branches. Do not rewrite published history, move existing release tags, force-push, or bypass branch protection.
- Keep upstream GitHub Actions workflows intact where possible. Make only the changes needed to validate and release this fork. Use the existing `ci.yml` and `release.yml`; Pages deployment uses GitHub Actions from the controller source on `main`.
- Obtain explicit user authorization before editing an existing GitHub Actions workflow. First present the reason and the exact proposed diff; do not work around this requirement with a release branch or a replacement release workflow.
- Keep native and Web controller source together on `main`. Commit source and lockfiles, not generated Pages assets. Build/test output belongs in ignored `build/` or existing component output directories; private runtime state, diagnostics and review records belong in ignored `local/`.
- Use `<upstream YY-MM-DD>-<fork revision>` for release names and tags, and `spice2x-<version>` for archive bases. `FORK_VERSION` is the version source; `UPSTREAM_BASE` records the exact upstream commit. Start at revision `1` for each new upstream release and increment for subsequent fork releases. Preserve historical tags and assets.
- Release only an exact commit on `main` whose CI passed. Include native and controller source, version/provenance metadata, and SHA-256 checksums. The unchanged upstream release workflow still uses date-based packaging; fork packaging is a local tool until its integration is explicitly authorized. Do not run that workflow to publish a fork-standard release or introduce a replacement workflow/branch.
- Keep README and setup documentation concise and limited to implemented fork additions, requirements and limitations. Do not put implementation status, build records or test results in this file.
- Preserve API, command-line and config compatibility. Scope XP-only graphics and shutdown behavior to `SPICE_XP`. Distinguish automated regressions from real cabinet/driver/input verification; claim support only within the verified scope.
