# Fork release history

New releases use `<upstream version>-<fork revision>` as their tag and title,
with `spice2x-<version>` archive bases. See the
[release workflow](Fork-workflow.md#releases) for source metadata and checksums.

The releases below share upstream source snapshot
[`c834967`](https://github.com/spice2x/spice2x.github.io/commit/c83496776e4dfa06f286e483d21fd269c331505c),
six commits after upstream release `26-10-03`. All are experimental prereleases.
Existing tags, download names and original assets are preserved; older packages
do not acquire the metadata or complete controller source added by the new
packager.

| Version / historical tag | Exact fork source | Change |
| --- | --- | --- |
| [`26-10-03-2`](https://github.com/miaulightouch/spice2x.github.io/releases/tag/26-10-03-2) | [`b1f2fdb`](https://github.com/miaulightouch/spice2x.github.io/commit/b1f2fdb918a79b6b65f178291577b2b8c3d852d0) | Shared configuration root; remove the separate resize path override |
| [`26-10-03-1`](https://github.com/miaulightouch/spice2x.github.io/releases/tag/26-10-03-1) | [`d9ff661`](https://github.com/miaulightouch/spice2x.github.io/commit/d9ff6619e68a73150c82ef786ded4a8122ed38af) | Portable/shared configuration and identity-transform bypass |
| [`26-10-16-xp-opengl.3`](https://github.com/miaulightouch/spice2x.github.io/releases/tag/26-10-16-xp-opengl.3) (legacy name) | [`a27620a`](https://github.com/miaulightouch/spice2x.github.io/commit/a27620a6b7d3c680723980e58e9dc05c625d6d58) | Save and restore the selected calibration scene |
| [`26-10-16-xp-opengl.2`](https://github.com/miaulightouch/spice2x.github.io/releases/tag/26-10-16-xp-opengl.2) (legacy name) | [`cfb7936`](https://github.com/miaulightouch/spice2x.github.io/commit/cfb79364912dffc28ecf64f21b66d3440da3ddc9) | Scope XP graphics and power behavior to `SPICE_XP` |
| [`26-10-16-xp-opengl.1`](https://github.com/miaulightouch/spice2x.github.io/releases/tag/26-10-16-xp-opengl.1) (legacy name) | [`74b1bb3`](https://github.com/miaulightouch/spice2x.github.io/commit/74b1bb30043001625a4d0e6b524a2dc6fa4cfeff) | XP power control and legacy jubeat OpenGL calibration |
| [`26-10-16-resize-api.1`](https://github.com/miaulightouch/spice2x.github.io/releases/tag/26-10-16-resize-api.1) (legacy name) | [`36bc9a0`](https://github.com/miaulightouch/spice2x.github.io/commit/36bc9a0fb751006cda4502b0eb5f1a66b69010d7) | Validated live resize API and Python wrappers |

Use the setup instructions belonging to the downloaded version. In particular,
the legacy `-resizecfgpath` override is absent from `26-10-03-2` onward; use
`-configdir` or `-portable` instead. The full ZIP includes XP extras; the XP-only
ZIP supplies replacement executables for an existing installation.

The controller's historical source was on `feature/web-controller`, with built
assets on `gh-pages`. New source is integrated on `main`; Pages deploys through
GitHub Actions. Historical releases are not evidence that native and Web source
were packaged together at that time.
