# Fork setup and usage

[Controller](https://miaulightouch.github.io/spice2x.github.io/) · [Downloads](https://github.com/miaulightouch/spice2x.github.io/releases) · [Upstream guide](https://github.com/spice2x/spice2x.github.io/wiki)

## Web controller

1. Install this fork's binary. For Windows XP, use the XP archive or `extras/winxp` executables.
2. Add `-api 1337 -apipass YOUR_PASSWORD` to the launch command.
3. Open the controller. In **Connection**, enter `ws://CABINET_IP:1338` and the same password. The WebSocket port is always the API port + 1.
4. Adjust **Screen**, then select **Save to cabinet** to keep all scenes and the selected scene after restarting.

Use Chrome or Edge and allow local-network access. If the browser requires `wss://`, provide a TLS gateway; the cabinet endpoint only supports `ws://`.

Cards and profiles stay in the browser. Passwords are saved only with **Remember password**. Demo mode sends no cabinet commands. Offline mode opens the interface; cabinet controls still need a connection.

## Logs and controls

Log access, automatic Service/Test release and startup display-mode logging require a cabinet build containing [PR #9](https://github.com/miaulightouch/spice2x.github.io/pull/9).

In **System**, **Refresh log** shows the latest 64 KiB; the filter matches lines. **Download log** retrieves the complete log at request time. An API password is required; only the current process's log is accessible.

Startup logs list available resolutions, refresh rates, color depths and interlace flags. `-sysdump none` disables this listing.

Service/Test releases automatically after 180 ms, including after disconnection. Older builds report an unsupported function. Game and power actions require a password and confirmation.

## Image calibration

Scaling (60–250%), offsets, aspect lock, filtering and duplication affect the game image. Windows resolution and window size are unchanged.

Supported paths: Direct3D 9, plus jubeat's legacy OpenGL on XP builds. Other paths may accept settings without rendering them. The grid and capture are previews; check the cabinet image.

Active XP OpenGL calibration may add latency; verify on the cabinet.

## Config paths and portable use

| Option | Settings location |
| --- | --- |
| Default | `%APPDATA%\spicetools.xml`; helper files under `%APPDATA%\spice2x\` |
| `-portable` | Replace `%APPDATA%` with the EXE directory; keep the same tree |
| `-configdir "E:\spice-config"` | Main XML and all helper files directly in that directory |
| `-cfgpath "E:\custom.xml"` | Override only the main XML |

Portable tree:

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

Priority: per-file override > `-configdir` > `-portable` > defaults. Use the same options for the configurator. Relative overrides use the working directory; environment variables are expanded. Copy existing settings yourself. Logs and game data keep their original paths.

Legacy helper files at the AppData/portable root remain supported; `spice2x/` takes priority. Calibration under `-configdir` always uses that directory's `spicetools_screen_resize.json`.

## Settings after restarting

Settings are per game. Keep the same Windows account, config path and game identity. After upgrading from a build that forgot the selected scene, select it and save once.

- Resets after a game restart: check the path and `ScreenResize` load/save log entries.
- Resets only after Windows restarts: check EWF/FBWF write filters and startup scripts. Use a writable location verified to survive reboot, then save and retest. A successful save cannot bypass a write filter.

[Resize API reference](../src/spice2x/README.md#resize)
