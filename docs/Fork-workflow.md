# Fork development and release workflow

This fork follows Proton-BM's separation of upstream provenance, focused fork
changes, versioned releases and private local records. spice2x keeps its own
source layout and directly edits source; it does not use Proton's patch stack.

## Source and upstream

- `origin`: <https://github.com/miaulightouch/spice2x.github.io>
- `upstream`: <https://github.com/spice2x/spice2x.github.io>
- `main`: integrated native source in `src/spice2x/`, Web controller source in
  `controller/`, documentation and release configuration.
- `UPSTREAM_BASE`: the exact upstream commit included in the fork.
- `FORK_VERSION`: `<upstream YY-MM-DD>-<positive fork revision>`.

The upstream date identifies the release family. The source snapshot may also
include later upstream commits; record that exact commit in `UPSTREAM_BASE`.
Increment the fork revision for the same upstream release. For a new upstream
release, reset it to `1`.

Keep generated native output in the existing `src/spice2x/bin/` and `dist/`
directories, Web output in `controller/dist/`, and assembled releases in
`build/release/`. These paths are ignored. Use ignored `local/` for cabinet state,
diagnostics, build logs and review records. Never commit credentials, game data
or machine-specific settings.

## GitHub flow

1. Fetch `origin`, start a bounded task branch from `origin/main`, and edit source
   directly. Agent branches use `codex/<task>`.
2. Add or update the relevant regression check. Keep native, controller, setup
   and API changes together when they depend on each other.
3. Push the task branch to `origin` and open a pull request targeting this fork's
   `main`. Do not push directly to `main` or to `upstream`.
4. Review the change and pass the required checks: `Build`, `Build (MSVC 32-bit)`,
   `Build (MSVC 64-bit)`, `Fork regressions`, and `Controller`. Keep the branch
   current with `main` before merging. Resolve review conversations.
5. Merge through GitHub after the checks pass. Deployments and releases use the
   integrated commit; do not maintain separate release source branches.

`main` protection must require pull requests and these checks, apply to admins,
and prohibit force pushes/deletion. For this single-maintainer fork, PR review
is recorded in the PR without requiring an approval from the PR author.

The existing `ci.yml` remains the upstream native build workflow. The added
`pages.yml` runs native fork regressions and controller tests/build. Existing
workflow changes require explicit user authorization after a concrete diff and
reason are presented.

## Upstream sync

`%pjsync` selects the most recent published upstream release, including a
prerelease, and performs a normal task-branch/PR sync. Fetch the release tag
from `upstream`, merge it on `codex/sync-<tag>`, reconcile fork changes and run
the checks. If it is already an ancestor of the recorded base, keep the newer
snapshot. Do not downgrade source or replay changes as patch files.

Update `UPSTREAM_BASE`, the upstream part of `FORK_VERSION`, and affected fork
documentation when the base changes. Resolve conflicts in the source and keep
the compatibility rules in [CONTRIBUTING.md](../CONTRIBUTING.md).

## Pages

Set **Settings → Pages → Build and deployment → Source** to **GitHub Actions**.
The `github-pages` environment should allow deployments from `main`.

The Pages workflow tests and builds `controller/` for pull requests and `main`.
Only `main` deploys, after its regression and controller jobs succeed. It uploads
a Pages artifact and deploys it with GitHub's Pages actions. Generated files are
not pushed to `gh-pages`; the old branch can remain as historical deployment
data. The public URL remains unchanged.

See GitHub's [custom Pages workflow documentation](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages)
and [publishing-source settings](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site).

## Releases

The existing manual `release.yml` is preserved byte for byte. It retains upstream's
date-based ZIP naming and moving `main` target, and does not invoke the fork
packager. Do not use it to publish a release under the fork convention. Integrating
fork packaging or exact-commit checks requires explicit authorization; there is
no replacement release workflow or release branch.

Before a future fork release, merge the version update by PR and let all `main`
checks pass. Its eventual release process must build that exact commit, use
`FORK_VERSION` for its tag/title, and create a draft prerelease for asset review.
Do not replace an existing tag or published asset. Hardware support remains limited to the verification
described in the setup documentation; automated tests do not prove cabinet
shutdown, legacy WGL driver behavior or input-to-display latency.

The local release packager can assemble an already-built, clean source checkout
for inspection. It is not connected to GitHub release publication:

```sh
python3 scripts/package_release.py --binaries src/spice2x/bin/spice2x
cd build/release
sha256sum -c SHA256SUMS
```

It requires regular and XP binaries and the matching commit source archive from
`build_all.sh`. Tracked changes, untracked source and stale build snapshots are
rejected; ignored build/runtime files are allowed. It emits:

| File | Contents |
| --- | --- |
| `spice2x-<version>.zip` | Regular distribution, version, provenance and complete source |
| `spice2x-<version>-full.zip` | Regular distribution and upstream extras, including XP binaries |
| `spice2x-<version>-winxp.zip` | XP 32-bit executables for an existing installation, license and metadata |
| `spice2x-<version>-source.tar.gz` | All tracked native/controller source, docs and build configuration |
| `RELEASE.json` | Fork version, source commit/tree and upstream version/commit |
| `SHA256SUMS` | SHA-256 checksums for the release files |

Historical tags and original binary assets remain available. See
[release history](Releases.md) for their source commits and naming conventions.
