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

## Config paths and portable use

Settings can be read and written anywhere the Windows account has permission:

| Launch option | Storage |
| --- | --- |
| `-configdir "%APPDATA%\spice"` | Main `spicetools.xml` and helper settings in that directory |
| `-configdir "E:\spice-config"` | The same files in any chosen directory |
| `-portable` | Replace the AppData root with the EXE directory, preserving the upstream layout |
| `-cfgpath "%APPDATA%\spice\custom.xml"` | Override just the main XML file |

`-configdir` and `-portable` are command-line options, resolved before the main XML is read. Use the same option when launching the configurator separately. Per-file overrides take precedence over `-configdir`, which takes precedence over `-portable`. The shared root covers main XML, resize, patch manager, card manager, controller presets and camera settings. Calibration always uses `spicetools_screen_resize.json` within that root. It does not redirect game NVRAM/data or logs.

Environment variables are expanded by the program, including paths entered without a CMD wrapper. Missing variables retain Windows' normal unexpanded spelling. Relative directory/file overrides keep working-directory semantics; `-portable` always uses the EXE directory. Required directories are created when possible. These modes do not read from the original AppData directory or migrate files automatically. For `-configdir`, copy the main XML and helper files directly into the chosen directory. For `-portable`, preserve their paths relative to AppData:

```text
EXE directory/
├── spicetools.xml
└── spice2x/
    ├── spicetools_screen_resize.json
    ├── spicetools_patch_manager.json
    ├── spicetools_card_manager.json
    ├── spicetools_presets.xml
    └── spicetools_camera_control.json
```

Existing helper files at the portable root remain supported; files under `spice2x/` take priority, matching upstream lookup.

Without these new options, upstream AppData and legacy lookup behavior is unchanged. A portable directory on a write-protected or write-filtered volume still needs an appropriate persistent location.

## Settings after restarting

By default, the cabinet stores calibration in `%APPDATA%\spice2x\spicetools_screen_resize.json`; an existing legacy `%APPDATA%\spicetools_screen_resize.json` may be used instead. Settings are per game. Keep the same Windows account, config path, and game identity when restarting.

New saves remember the selected scene. Older releases always started on Scene 1 even if another scene was saved. After upgrading, choose the intended scene and save once. Old config files without a selected-scene field continue to start on Scene 1.

Windows XP Embedded may use EWF or FBWF. Writes to a protected volume can succeed in a temporary overlay and disappear at a full reboot; this behavior is not specific to Konami. See [Microsoft's write-filter overview](https://techcommunity.microsoft.com/blog/microsoft-security-blog/managing-embedded-devices-with-write-filters-in-configuration-manager-service-pa/247561/).

To identify which case applies:

- Restart only the game after saving. If it already resets, check the selected scene, launch account/path, and the log's `ScreenResize` load/save entries.
- If a game restart retains settings but a Windows reboot restores or removes the JSON file, check the cabinet's write-filter configuration and startup scripts.
- Store settings in a location verified to survive reboot, for example by adding `-configdir "E:\spice-config"` to the launch command. The calibration file is `spicetools_screen_resize.json` in that directory. Create/use your own persistent directory; `E:` is only an example and is not necessarily unprotected. Save again, then verify a reboot.

The application does not change EWF/FBWF settings. A successful save means the OS accepted the file write; it cannot guarantee that a write filter or startup script will retain it after reboot.

## API

The fork adds `resize.image_resize_get`, `resize.image_resize_set`, and `resize.image_resize_save`; see the [API reference](../src/spice2x/README.md#resize). The save API preserves existing window settings. Scene persistence and validation are shared code on all Windows builds; the WGL hook and XP power flags remain guarded by `SPICE_XP`.

## OpenGL latency

The XP OpenGL transform runs immediately before the existing buffer swap. It does not queue or deliberately delay a frame. Active scaling currently copies the framebuffer into a GPU texture, redraws it in a separate WGL context, and uses two `glFinish()` synchronization points to preserve ordering across contexts. These waits serialize work and can increase frame time; missing a refresh deadline can add a refresh interval of display latency. They may also reduce existing GPU queue depth, so CPU wait time is not a direct measurement of input-to-display latency.

An identity scene (100% on both axes, zero offsets, no duplication) now bypasses the extra copy, draw, context switches and GPU waits. Filtering and aspect-lock flags alone do not prevent this bypass. Active transforms cache context-constant texture/buffer capabilities and avoid reapplying unchanged texture parameters. They retain the tested synchronization and compatibility path. A same-context pass could avoid cross-context waits, but requires preserving the game's shader/FBO/texture/fixed-function state and validating the legacy ATI driver. Simply removing `glFinish()` or replacing it with `glFlush()` is not a proven-safe latency fix. No millisecond or zero-latency claim is made without cabinet measurements.
