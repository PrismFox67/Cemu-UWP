# Changelog

What changed in this fork, newest first, written for players. Every push to `d3d12-backend` becomes a new
[ci-latest](https://github.com/PrismFox67/Cemu-UWP/releases/tag/ci-latest) build; the release notes there show the newest
entries of this file plus every commit since the previous build, so you can see what's new since you last looked. The
Xbox package of each build has a higher version (`0.1.<build>.0`), and the launcher says when a newer build is available.

## 2026-11-10
### Xbox launcher
- Skylanders: the figure list in the in-game menu is grouped by game in release order (Spyro's Adventure, Giants, Swap Force, Trap Team, SuperChargers, Imaginators) and alphabetical within each game; the menu shows the figure's game and its place in the list.

## 2026-11-09
### Xbox launcher
- The "closed unexpectedly" notice above the game list is smaller. Down moves to the game list without dismissing it, so you can start a game right away; up goes back. While its buttons have the focus, left/right only moves between them and no game looks selected.
- Skylanders: several figures can stand on the portal at once (two players, traps, Swap Force top and bottom halves). A on a figure in the in-game menu adds it or takes it off; the others stay. To swap a figure, take the old one off first.

## 2026-11-07
### Mario Kart 8 first person pack
- The character and kart screens show your whole driver again: the first person view and the driver hiding now only apply during races and battles.
- Bowser on bikes: his arms no longer block the cockpit view; only his hands stay on the handlebars.
### Games
- Breath of the Wild: fewer multi-second freezes when opening the map, selecting a rune or teleporting on Xbox. The game kept asking to reload its big terrain textures with unchanged contents, which took about 3.5 s each time on Xbox; unchanged parts are now skipped. Measured on PC, still needs confirming on Xbox (GitHub #5, #12).
### Xbox app
- The loading screen has one bar from 0 to 100% for shaders and pipelines together, instead of starting over for the pipelines. The caption still says which step is running. How much of the bar each step gets comes from how long it took the last time you started that game.
## 2026-11-06
### Xbox app
- LEGO Dimensions: the Toy Pad now turns on by itself when the game starts, so there is no first-time restart. Other games still only get it if you turn it on in the menu. Not tried with the game yet (GitHub #15).

## 2026-11-05
### Games
- **Broken letters:** block compressed texture arrays are now written by the CPU on Xbox, because GPU copies put every layer after the first in the wrong place. A tester confirmed clean Arabic text in Breath of the Wild on Xbox (GitHub #12). Wind Waker HD pause letters (GitHub #9) use the same kind of font texture but haven't been checked yet. Adapted from danprice142/Cemu-UWP.
- Breath of the Wild shadows: three testers report them right on Xbox since this change (GitHub #5). The cause isn't proven.
### Xbox app
- Logging only: the log now shows how long texture uploads wait for the GPU, to help track down the Breath of the Wild freezes when selecting a rune, opening the map or teleporting (GitHub #5). No fix yet.
- LEGO Dimensions Toy Pad in the in-game menu (GitHub #15): pick one of the 7 spaces, then place any built-in figure or your own .bin dump from the dimensions folder, no real figures needed. Only the menu has been tested, on PC; it hasn't been tried with the game or on Xbox yet.
- A game that closes itself no longer crashes Cemu: you go back to the game list with a message. DS Virtual Console games need Mii system files from your own Wii U (system title 0005001b-10056000); without them they stop at startup and Cemu now tells you so.
- Quick start no longer competes with the game when memory runs low: background pipeline creation waits a minute after memory pressure and stops for the session after the third time, and what the loading screen built is kept unless memory is critical.
- Crash logs: when a game thread crashes, the log now shows more about the code that was running, to help track down the Sonic Lost World crash (GitHub #12). Logging only.
### Site and CI
- **Older versions:** every Xbox build is now kept on the [older versions page](https://github.com/PrismFox67/Cemu-UWP/releases/tag/older-versions), no GitHub account needed, so you can go back and find the build where something changed. New builds are added automatically.

## 2026-11-04
### D3D12
- Shadows: shadow-map (comparison) samplers no longer use anisotropic filtering, a possible cause of blocky or doubled shadows (GitHub #5, #8, not confirmed on Xbox yet). For testing, an empty `depth_d32.txt` in the user folder stores depth in a higher-precision format.
- Shadows: slope-scaled depth bias was rounded to 0 for small values (Breath of the Wild's shadow casters got none), a likely cause of blocky, doubled or missing shadows (GitHub #5, #8). Adapted from Dushe22/Cemu-UWP.
- **Less pop-in with "New shaders: Smooth"**: while a new pipeline is created, the draw uses a ready one with the same shaders and targets (only blend factors, depth/stencil tests, culling or depth bias differ) instead of being skipped. Draws with stream-out are never skipped now.
### Games
- Text and sprites whose game asks for sharp (point) magnification are no longer blurred by the D3D12 backend when anisotropic filtering is set.
### Xbox app
- **Save logs / Back up saves to USB work without a game folder**: if your games are installed instead of in a game folder, Cemu now finds the USB drive by itself (GitHub #12).
- **Quick start is on by default** (Settings, pipelines): the loading screen only creates the pipelines the menus and the start of play needed last time, the rest are created in the background while you play. Turned on once for existing settings; turn it off again if you prefer the full wait.
- **Save folder: "Use Cemu's own storage" button** next to Set save folder, so you can go back without typing (GitHub #12).
- Smoother under memory pressure: preloaded pipelines that no draw has used yet are released when memory runs low, and the
  graphics thread runs at a higher priority than the emulated CPU threads.

## 2026-11-03
### Games
- Breath of the Wild: missing or flickering short grass (vertex data past the size the emulator computed read as 0). Adapted from Dushe22/Cemu-UWP.
- Breath of the Wild: when memory gets tight, the large terrain textures stay loaded while they were used in the last ~2 minutes instead of being reloaded slice by slice. Adapted from Dushe22/Cemu-UWP.

### Xbox app
- **CPU mode setting** (Settings -> CPU, GitHub #11): Automatic (default, multi-core with 4+ cores), Single-core recompiler
  (most compatible) or Multi-core recompiler. A game's own profile in `gameProfiles` still wins.
- **GamePad overlay: two new positions.** "Center" puts it where the lines of a 2/4-player split screen cross; "Side
  by side" shrinks the TV picture to the left and shows the GamePad screen next to it, nothing covered (size sets how
  much). The overlay defaults for games without their own setting are now in Settings → GamePad screen overlay.

## 2026-11-02
### Xbox app
- **No more stutter when new things appear**: Settings -> Graphics -> New shaders is now **Smooth** by default. Before,
  the game paused up to 2 seconds whenever it needed a new shader (the Xbox takes ~2.5 s to make each one); now it keeps
  running and a new effect can pop in a moment late. If you had Balanced saved (the old default), you are moved to
  Smooth once; pick Balanced or Accurate again in Graphics if you prefer it.

### Graphic packs
- **MK8 first-person camera, split screen**: in 2-4 player races each player switches their own view with R3
  (player 1 on the GamePad, players 2-4 on Pro Controllers); before, player 1's R3 changed everyone's camera. Every
  player's head is hidden, so no moustache in view. Big drivers (Bowser, Dry Bowser, DK, Wario, Waluigi, Tanooki
  Mario) are now detected automatically, no separate preset needed. Downhill sections now tilt the view as intended.
  Known: bikes and ATVs still put the eye in a bad spot for some drivers (being worked on).

## 2026-11-01
### Xbox app
- **Second controller works in more games** (e.g. Super Smash Bros.): players 2-4 can no longer be a "Wii U GamePad",
  which almost no game reads, so player 2 looked connected but did nothing. Players 2-4 that were a GamePad become a
  Pro Controller once, on the same controller.

## 2026-10-31
### Xbox app
- **Easier second controller**: press A on a controller that no player uses and the app asks "press A again to join as
  player 2" (or the next free player); the second A adds it. Controllers still never join on their own. Pair
  controllers (menu) and the Controllers tab work as before.

## 2026-10-30
### Games
- **Zelda BOTW: Magnesis, Stasis and Sheikah Slate pins now work on Xbox** (GitHub #2): the Xbox graphics driver lost
  data in one of the shaders BOTW uses to find what the reticle points at, so rune targets never locked on and scope
  pins landed at Link's feet. That shader is now rewritten into a form the driver handles. Confirmed on Xbox Series X
  and Series S by testers (build 0.1.139). PC builds are unchanged.
### Xbox app
- **Dead analog sticks fixed** (GitHub #2): picking a player's controller type or controller again on the Controllers
  tab (or pairing) left both sticks without a mapping, and that stayed after a restart. Choosing the same type again
  now changes nothing, and broken profiles get their stick mappings back once at start.

## 2026-10-28
### Games
- **Zelda BOTW: Magnesis works again** (GitHub #2): metal objects turn yellow and can be grabbed. The cause was the
  three Pro Controllers the app connected for players 2-4 even with no controller behind them. Now only player 1 (the
  GamePad) is connected, and the default players 2-4 are switched off once on update. For multiplayer add players with
  **Pair controllers** in the in-game menu or on the Controllers tab (leave them off in BOTW). If you had set player 1 to Pro Controller yourself, it is switched back to the GamePad once:
  a Pro Controller on player 1 breaks Magnesis too.
- Games no longer crash when they point a texture at memory that doesn't exist (seen in Sonic Lost World right after the
  title screen on Xbox); the part that exists is used and the rest left blank (only textures with no data at all are skipped), and it is noted
  in the log. Sonic Lost World no longer crashes; whether its title screen background now shows is not confirmed yet.
### Xbox app
- **Save folder on the USB drive** (GitHub #6): Settings → Folders → Save folder (for example `E:\CemuSaves`) keeps
  your saves there, so they survive a Dev Mode reset and every Xbox profile shares them. Your current saves are copied
  over (never moved), restart Cemu afterwards. Without the drive Cemu uses its own storage for that session and says so.

## 2026-10-24
### Mario Kart 8 first person pack
- **Cockpit view for big drivers:** a new View preset, "Cockpit, big drivers (Bowser, Dry Bowser, Wario, Waluigi, DK,
  Link...)", hides the driver so their arms, legs or body no longer block the view; the kart and wheel stay. Mario-size
  and small drivers keep the normal cockpit view with hands on the wheel.

## 2026-10-23
### Direct3D 12
- **BOTW: Magnesis and Stasis pick their target again.** The game's target check (an invisible probe drawn against the
  scene depth) never passed on Direct3D 12, so metal objects stayed pink and A did nothing. The probe now always passes;
  tested on PC: the target turns yellow and is grabbed. Side effect until the real cause is found: a target behind a
  wall can be picked too.
- **Fewer pipelines for depth-biased draws (shadows):** depth bias values that differ only slightly now share one
  pipeline instead of each building its own, which should mean fewer new-shader hitches in games that animate their
  shadow bias (reported for BOTW).
- **A shader the game is waiting for comes first:** while a draw waits for its pipeline, background shader builds pause
  and the waiting one runs at normal priority, so freezes like opening a new menu or ability should be shorter.

### Xbox app
- **GamePad overlay:** the in-game menu can show the GamePad screen small in a corner of the TV image (bottom right by
  default, about the size of MK8's item box), so the MK8 minimap and other GamePad-only screens are visible on the TV.
  Corner, size (small/medium/large) and opacity can be changed; each game remembers its setting, off unless you turn it
  on. With the GamePad screen shown full screen, the overlay shows the TV instead.
- **keys.txt from anywhere:** Settings > Folders has a keys.txt path (a file, or a folder with keys.txt, e.g. on a USB
  drive). New keys from it are imported right away and at every start; a missing file is reported.
- **Fixed: starting a game a second time in the same session crashed with graphic packs** that patch code (FPS++,
  Graphics, ...): the patches were applied again to the first run's freed game module and their code caves never
  freed. After a crash, the next start now says it crashed instead of "closed while suspended".
- **Real box art:** the game list shows the Wii U cover of each game, downloaded in the background from GameTDB
  (high-resolution where available) and kept in `covers` in the user folder. Turn it off or re-download under
  Settings > Box art. Without internet the icon cards are shown as before.
- **Menu sounds:** soft clicks for moving, select, back and switching tabs, and an error sound (Kenney's CC0
  Interface Sounds). Settings > Look: on/off and volume; your own `move`, `select`, `back`, `tab`, `error` .ogg/.wav
  in `LocalState/theme/sfx` replace them.
- **Menu music:** put your own `menu_music.ogg`, `.mp3`, `.wav` or `.flac` into `LocalState/theme` and it loops on
  the menu screens (fades out when a game starts). Settings > Look: on/off and volume (60% by default). Nothing is
  bundled, without a file the menu stays silent.
- **About** names PrismFox67 as the author of Cemu UWP, with the Cemu team and danprice142 credits below.
- **Dark panel by default** (Settings > Look still offers the light one; a light choice made before is kept).
- **Bigger covers, no titles under them:** the selected cover takes about 45% of the screen height, the others about
  37%; the selected game's title leads the info line below, shortened with "..." instead of wrapping.
- **The Cemu logo** (grey "Cem" + cyan "U" on its navy gradient) replaces the plain text title on the home screen.

### Mario Kart 8 first person pack
- **The view follows downhill slopes and glider dives sooner** (after about 1/3 s instead of 3/4 s, and catches up
  faster), so the road ahead stays in view going down hills and in the air. Throwing items still doesn't dip it.

## 2026-10-22
### Xbox app
- **In-game menu is easier to find and does more:** when a game starts, a hint shows for a few seconds how to open it
  (click both sticks, or hold View + Menu). The menu now also has VSync, volume and player 1's stick deadzone; change
  them with left/right, they apply right away.

### Direct3D 12
- **BOTW Magnesis with the BotW Graphics pack (1080p and other resolutions):** the pack scales one small texture that
  the rune's targeting shares with an unscaled one, and the copy between them was skipped, so metal objects weren't
  found. Such copies are now drawn with scaling instead. Please report whether Magnesis highlights and grabs now.
- **BOTW Magnesis / Stasis, second attempt:** BOTW tests what its runes can target against a small copy of the depth
  buffer. That copy covered only part of the buffer, which Direct3D 12 can't do with a plain copy, so it was dropped and
  every test counted "hidden". Such copies are now drawn. Occlusion test results also reach the game within a few draws
  instead of at the end of a frame, and draws inside a test always wait for their shader instead of being skipped.
  Please report whether Magnesis and Stasis find their targets now.

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
