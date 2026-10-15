# Fork setup and usage

These instructions cover this fork's additions. See the [upstream wiki](https://github.com/spice2x/spice2x.github.io/wiki) for general spice2x behavior.

## Web controller

Open [Arcade Room](https://miaulightouch.github.io/spice2x.github.io/). The language selector supports English, Traditional Chinese, Simplified Chinese, Japanese, and Korean. The initial language follows the browser; your selection is remembered on that browser.

1. Use a binary from this fork's [releases](https://github.com/miaulightouch/spice2x.github.io/releases). On Windows XP, use the XP archive or `extras/winxp` executables from the full archive.
2. Add `-api 1337 -apipass YOUR_PASSWORD` to your existing launch command. The WebSocket port is the API port plus one: `ws://CABINET_IP:1338` in this example.
3. Connect from a device that can reach the cabinet; allow the browser's local-network permission when prompted. Enter the endpoint and the same API password in Connection.
4. Adjust the image under Screen. Changes apply immediately; **Save to cabinet** writes all four scenes, the active scene, enabled state, and filtering to the cabinet's config file.

The controller also provides service/test pulses, button release, coins, saved cards, P1/P2 keypads, game restart/exit, and Windows restart/power-off. Game and power actions require an API password and confirmation. A dropped connection does not retry an action. A cabinet startup script may relaunch a game after Exit.

Profiles and cards stay in the current browser. Passwords are only stored when Remember password is selected. The explicit demo mode sends no cabinet commands. The cached interface can open offline; cabinet operations require network access.

Prefer a current Chrome or Edge with local-network permission for an HTTPS page connecting to a private-IP `ws://` endpoint. Where the browser requires TLS, supply a valid `wss://` gateway; spice2x's native WebSocket endpoint does not provide TLS. See [Chrome's local-network guidance](https://developer.chrome.com/blog/local-network-access).

## Image calibration

The transform changes the game image, not the Windows window size or output resolution. It supports 60–250% scaling, pixel offsets, aspect lock, linear filtering, and left/right duplication. Editing or selecting a scene in the Web controller enables resizing; explicitly disabling it can also be saved.

Direct3D 9 uses the existing rendering path. `SPICE_XP` builds additionally support jubeat's legacy OpenGL path using OpenGL 1.1 and a separate WGL context. Non-XP builds do not compile or initialize that backend. The driver must support a power-of-two texture large enough for the image. Diagnostic messages use `graphics::opengl` in the log.

Other rendering paths can accept settings without displaying the transform. OpenGL capture and overlay support are unchanged. The grid is a preview, and cabinet capture is a manually refreshed still image; neither is proof that the game rendered the saved transform.

## Settings after restarting

By default, the cabinet stores calibration in `%APPDATA%\spice2x\spicetools_screen_resize.json`; an existing legacy `%APPDATA%\spicetools_screen_resize.json` may be used instead. Settings are per game. Keep the same Windows account, config path, and game identity when restarting.

New saves remember the selected scene. Older releases always started on Scene 1 even if another scene was saved. After upgrading, choose the intended scene and save once. Old config files without a selected-scene field continue to start on Scene 1.

Windows XP Embedded may use EWF or FBWF. Writes to a protected volume can succeed in a temporary overlay and disappear at a full reboot; this behavior is not specific to Konami. See [Microsoft's write-filter overview](https://techcommunity.microsoft.com/blog/microsoft-security-blog/managing-embedded-devices-with-write-filters-in-configuration-manager-service-pa/247561/).

To identify which case applies:

- Restart only the game after saving. If it already resets, check the selected scene, launch account/path, and the log's `ScreenResize` load/save entries.
- If a game restart retains settings but a Windows reboot restores or removes the JSON file, check the cabinet's write-filter configuration and startup scripts.
- Store the resize file on a location verified to survive reboot. Add the existing path override to the same launch command, for example `-resizecfgpath "E:\spice-config\screen-resize.json"`. Create/use your own persistent directory; `E:` is only an example and is not necessarily unprotected. Save again, then verify a reboot.

The application does not change EWF/FBWF settings. A successful save means the OS accepted the file write; it cannot guarantee that a write filter or startup script will retain it after reboot.

## API and development

The fork adds `resize.image_resize_get`, `resize.image_resize_set`, and `resize.image_resize_save`; see the [API reference](../src/spice2x/README.md#resize). The save API preserves existing window settings. Scene persistence and validation are shared code on all Windows builds; the WGL hook and XP power flags remain guarded by `SPICE_XP`.

Web source is in `controller/` on the [`feature/web-controller`](https://github.com/miaulightouch/spice2x.github.io/tree/feature/web-controller/controller) branch. Built assets are deployed from `gh-pages`. See that directory's README for local development. This fork does not require changes to upstream GitHub Actions on the main branch.
