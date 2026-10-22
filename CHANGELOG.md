# Changelog

What changed in this fork, newest first, written for players. Every push to `d3d12-backend` becomes a new
[ci-latest](https://github.com/PrismFox67/Cemu-UWP/releases/tag/ci-latest) build; the release notes there show the newest
entries of this file plus every commit since the previous build, so you can see what's new since you last looked. The
Xbox package of each build has a higher version (`0.1.<build>.0`), and the launcher says when a newer build is available.

## 2026-10-21
### Xbox app
- **New launcher.** A console-style game row: flat 2D covers with each game's icon art on a Wii U-menu-style background
  (sky-to-white gradient with drifting icons). **LB / RB** switch between Games, Graphic packs, Controllers, Settings and
  About; **A** starts the game, **X** refreshes the list, **Y** opens Settings. The line under the covers shows the region,
  title ID, version and whether an update or DLC was found. Button hints use the controller's stick and button symbols.
- **Settings → Look:** light or dark panel; your own background with `LocalState/theme/background.png` (or `.jpg`).
- **Settings → Language and region:** console language (the Wii U system language games use for text and voices),
  console region (Auto follows the game) and the launcher's interface language: English, Português (Brasil), Español.
  More texts or languages can be added with `LocalState/translations/<code>.txt`.
- **Settings → Install updates and DLC:** installs updates, DLC and games from a folder into the emulated Wii U storage,
  with a plan (type, version, size) before it starts. Adapted from danprice142/Cemu-UWP.
- **More game folders:** `D:/DevelopmentFiles/Games/wiiu`, `E:/Games/wiiu` and `E:/WiiU/games` are scanned when they
  exist, without having to add them.
- **Memory:** the process heap is compacted while memory pressure is on, returning freed pages to the Xbox.
- **Tester guide** updated for the new launcher, Look, Language and region, and translation overrides.

### Direct3D 12
- **BOTW Magnesis / Stasis (Xbox):** occlusion query results are now read through a ranged map so the GPU's answer is
  visible to the CPU on the Xbox's non-coherent memory. This is the suspected cause of Magnesis not highlighting objects;
  please report whether it works for you.
- **Out-of-memory recovery:** a texture allocation that fails is retried after releasing textures unused for 3+ frames
  (cold and large ones first).
- **No crash when a command list fails to close:** its work is dropped and the game goes on, instead of the app closing.
- **GPU watchdog:** a stall of the GPU is logged after 2 s (then every 5 s), so hangs leave a trace in the log.
- **Pipeline usage history per title:** pipelines a game hasn't used for 20 sessions are no longer rebuilt at launch,
  which shortens the "pipelines" loading after many sessions.
- **Mario Kart 8 font / icons experiment:** 2D array texture layers are rounded explicitly when sampled; optional
  texture dump (`dump_textures.txt` in the user folder) for the icon investigation.
- **Flip pacing experiment** (`flip_pacing.txt` in the user folder), adapted from danprice142/Cemu-UWP.

### Site and CI
- **Download counter:** a running total of the Xbox app downloads across builds (badge in the README, line in the
  ci-latest notes), refreshed after every build and every 6 hours. GitHub's own counter resets on each new build.
- Card image for the Emulation Revival listing.

## 2026-10-18
### Xbox app
- **Controllers:** per-stick deadzone up to 90 % with a live readout of the stick's current offset (for drifting
  sticks), a **Nintendo layout** preset (A↔B, X↔Y) with 3 profile slots per player, single-button remapping, and a
  mappable **Blow into mic (hold)** for Super Mario 3D World and others. A bug that reverted the Nintendo layout on every
  start is fixed.
- **Motion controls:** tilt the GamePad with the right stick (in-game menu → **Motion**) for gyro puzzles, or use a
  phone's real gyro through a DSU / cemuhook app (Controllers → Phone motion IP).
- **amiibo** (dumps in `LocalState/amiibo`) and a **Skylanders portal** (figures in `LocalState/skylanders`) in the in-game
  menu.
- **Wii U dialogs with a controller:** the on-screen keyboard (naming a character, Pokkén, Wind Waker HD) and error
  dialogs can be used with the controller; A closes error dialogs.
- **Graphic packs apply again on the Xbox** (a path bug stopped the bundled packs from installing), packs re-apply when
  a game is relaunched in the same session, and an unreadable pack folder no longer stops the others from loading.
- **Log and save copies** to the USB drive are flushed, so they're no longer 0 bytes.
- After a GPU reset, relaunching says **"Restart Cemu to play again"** instead of failing silently.
- **Compatibility list** (`docs/COMPATIBILITY.md`) started from tester reports.

### Direct3D 12
- Copies of 2D texture slices are refreshed on any GPU write (texture-to-texture copies were missed), fixing stale
  textures in some games.

### Mario Kart 8 first person pack
- Throwing items and drift hops no longer dip the first-person view; quick camera dips are filtered.
- `MK8_FirstPerson.zip` on the release is rebuilt on every push (the old copy had gone stale).

## 2026-10-13
### Xbox app
- **First-run setup screen**, a **crash notice** on the next start (the crash is in `log_previous.txt`), **log and save
  copies** to the USB drive, a **"new build available"** check, box art in the game list, stick range calibration.
- **keys.txt** is imported from the game folders.
- **Button mapping screen** per player.
- The save backup no longer stops silently after the first file error.

### Mario Kart 8 first person pack
- The pack ships with every build (Xbox app and both Windows builds); the locked camera keeps its pitch when throwing
  items.

### Site
- README describing the fork, an **Xbox / D3D12 bug report form** that asks for `log.txt` and `log_previous.txt`, and the
  tester guide instead of the Cemu Discord in the issue links.

## 2026-10-10 to 2026-10-12
### Direct3D 12
- **Stutter control:** "Smooth" mode never compiles a shader on the render thread (a new effect pops in a frame late
  instead of freezing the game); pipelines are created in the background with more compile threads; each level's
  pipelines are remembered and created on its loading screen the next time; **Quick start** creates only the startup
  pipelines on the loading screen; the wait for a new pipeline is capped; pipelines that crashed the driver are skipped.
- **Xbox pipelines:** no pipeline library on the Xbox (the Dev Mode driver removes the device), pipelines are kept in
  memory across sessions where possible, "New shaders" setting (Smooth / wait).
- **Memory:** unused textures are released between draws under memory pressure (all of them above 90 % of the budget),
  uploads that overflow the upload ring are submitted early, fewer pipelines are compiled at once near the limit, a
  texture memory breakdown is logged when pressure starts.
- **Frame interpolation** (120 FPS output from 60 FPS games) for PCs with a 120 Hz display, with the real refresh rate
  measured and interpolation paused under 100 Hz.
- Shader compiler choice (**DXC**, the default and about twice as fast to start games, or FXC), the runtime's implicit
  shader cache, a slim loading bar for the shader cache, texture copies kept inside both mips and slices, D3D12 debug
  layer messages in the log, `CEMU_D3D12_WARP=1` for the software adapter.

### Xbox app
- **In-game menu** (click both sticks): switch to the GamePad screen, **touch cursor** (right stick moves, RT taps),
  FPS counter, **Exit to game list**.
- **Audio settings** with 5.1 surround; full volume for new installs.
- **Graphic pack tab** with the community pack download; update version and DLC shown in the game list.
- A/B/X/Y mapped to the Xbox buttons with the same label; the keyboard no longer takes player 1's controller buttons;
  disconnected controllers reconnect; stick deadzone setting; file paths over 260 characters work.
- The app's memory use and budget are logged while a game runs; the log file is created at startup.
- **Xbox tester guide** (`docs/XBOX_TESTERS.md`): install, recommended settings, first launch.

## 2026-10-06 to 2026-10-08
- **Direct3D 12 renderer** added to Cemu (shaders translated with SPIRV-Cross and compiled with FXC/DXC, geometry
  shaders, pipelines created asynchronously so a slow driver compile can't freeze the game, pipelines cached per title
  on PC and precompiled on the loading screen, device-removed diagnostics).
- **Xbox (UWP) build** with a wxWidgets-free frontend: players 1-4 with controller pairing, keyboard as player 1 on PC,
  the in-game menu swaps the TV and GamePad screens, 128 MB stacks per thread.
- **CI:** every push builds the Windows D3D12 build, the Win32 host and the Xbox package onto the ci-latest release,
  with build logs and debug symbols.
