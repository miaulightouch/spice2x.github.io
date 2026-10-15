# Arcade Room

A compact WebSocket controller added by this [spice2x fork](https://github.com/miaulightouch/spice2x.github.io).

- **Calibration:** Four scenes, live scaling/offsets, aspect lock, filtering, duplication, and cabinet-side saving.
- **Cabinet controls:** Service/test pulses, button release, coins, cards, P1/P2 keypads, game restart/exit, and Windows restart/power-off.
- **Languages:** English, Traditional Chinese, Simplified Chinese, Japanese, and Korean. Browser-language detection with a remembered manual choice, including feedback, errors, and confirmation dialogs.
- **Compact layout:** Two columns on desktop, one on phones, 44px minimum button targets, and expandable preview/help.
- **Local profiles:** Multiple cabinets, browser-local cards, optional password storage, explicit offline demo, and cached interface assets.

Open the [controller](https://miaulightouch.github.io/spice2x.github.io/) and see [fork setup and usage](https://github.com/miaulightouch/spice2x.github.io/blob/main/docs/Fork-setup.md). Commands go directly from the browser to the cabinet; no relay server is required. Calibration requires this fork's Resize API. XP jubeat OpenGL requires the fork's XP build; capture support is separate.

## Development

Use Node.js 22.12 or later:

```sh
cd controller
npm ci
npm run dev
npm test
npm run build
```

Production assets are bundled locally in `dist/`, with a relative base path for project Pages. Tests cover binary NUL framing, the shared RC4 stream and rekey, request serialization, errors/timeouts, calibration queues, system actions, locale coverage, and locale persistence. Native server errors remain verbatim; protocol identifiers and user-saved names are not translated.

Add translations in `src/locales/`; all five catalogs must have identical keys and interpolation tokens. `src/i18n.ts` handles locale resolution, reactive feedback, and blocked browser storage without an additional runtime dependency.

## GitHub Pages

Source lives on `feature/web-controller`; built assets live at the root of `gh-pages`. Configure Pages as **Deploy from a branch → gh-pages → / (root)**. No changes to upstream GitHub Actions are required.

To publish from an authenticated checkout whose `origin` points to this fork:

```sh
npm run deploy:pages
```

The deployment script builds and updates `gh-pages` without force-pushing. The interface uses a service worker; after a deployment, reload the page to receive current assets. Machine writes are not retried automatically after a disconnect.

This directory uses the repository's GPL-3.0 license. Bundled notices are in `public/LICENSE.txt` and `public/THIRD_PARTY_NOTICES.txt`.
