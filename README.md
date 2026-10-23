# Cemu UWP: Cemu with Direct3D 12, for Xbox Series X|S and Windows

**This is an unofficial fork of [Cemu](https://github.com/cemu-project/Cemu).** It adds a native **Direct3D 12 renderer**
and an **Xbox app (UWP) frontend made for Dev Mode**, so Wii U games run on an Xbox Series X|S. It isn't affiliated with
the Cemu team; please report problems with this build [here](https://github.com/PrismFox67/Cemu-UWP/issues), not to them.

On a Series X it runs **Mario Kart 8 at 1440p or 4K, locked at 60 FPS**, with 4 local players. Other games:
**[compatibility list](https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/docs/COMPATIBILITY.md)**.

### Download
![Xbox app downloads](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fgithub.com%2FPrismFox67%2FCemu-UWP%2Freleases%2Fdownload%2Fdownload-stats%2Ftotals.json&query=%24.xbox&label=Xbox%20app%20downloads&color=107c10&cacheSeconds=3600)
(all builds since 2026-10-19; the number is kept across builds, GitHub's own counter resets on every new build)

Everything is on the **[ci-latest pre-release](https://github.com/PrismFox67/Cemu-UWP/releases/tag/ci-latest)**, rebuilt
on every change:

| File | What it is |
|---|---|
| `cemu-uwp-x64.zip` | **The Xbox app.** Install it through the Xbox Device Portal. Setup and recommended settings: **[Xbox tester guide](https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/docs/XBOX_TESTERS.md)** |
| `cemu-d3d12-windows-x64.zip` | Regular Cemu for Windows with the Direct3D 12 renderer added |
| `cemu-host-win32-x64.zip` | The Xbox frontend as a Windows program, for testing it on a PC |
| `MK8_FirstPerson.zip` | The Mario Kart 8 first person pack on its own, for other Cemu builds (the downloads above already include it) |

### Game compatibility
Games players have tested on Xbox Series X|S; the **[compatibility list](https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/docs/COMPATIBILITY.md)**
has the resolution, FPS and notes for each one.
- **Perfect:** Mario Tennis: Ultra Smash, New Super Mario Bros. U
- **Playable:** Mario Kart 8, Pokkén Tournament, Super Smash Bros. for Wii U, The Legend of Zelda: Wind Waker HD
- **Issues:** The Legend of Zelda: Breath of the Wild (Magnesis can't pick things up yet, blocky shadows), Super Mario
  3D World (graphical bugs), Splatoon
- **Broken:** Need for Speed: Most Wanted U (closes after the intro video)

Tried a game that isn't listed? Tell us how it went with the bug report form in the
[issues](https://github.com/PrismFox67/Cemu-UWP/issues) (it works for "it runs fine" reports too), and say which build
you used.

### What's new
**[CHANGELOG.md](CHANGELOG.md)** lists every change by date, written for players. The
[ci-latest release notes](https://github.com/PrismFox67/Cemu-UWP/releases/tag/ci-latest) show the newest entries plus
every commit since the previous build, so you can see what changed since you last looked. Each build's Xbox package has a
higher version (`0.1.<build>.0`), installs as an update over the previous one, and the launcher says when a newer build is
available.

### Features
#### The Xbox app
- **A launcher made for a controller.** A row of real Wii U box art (downloaded automatically from GameTDB, icon cards
  when offline) on a Wii U-menu-style background with the Cemu logo. **LB / RB** switch between Games, Graphic packs,
  Controllers, Settings and About; **A** starts the game, **X** refreshes the list, **Y** opens Settings. One line under
  the covers shows the selected game's title, region, title ID, version and whether an update or DLC was found. Dark
  panel by default (light in Settings → Look), your own background image, and your own menu music
  (`LocalState/theme/menu_music.ogg`, `.mp3`, `.wav` or `.flac`; nothing is bundled).
- **Games from a USB drive or the internal drive.** The usual game folders are scanned automatically, `keys.txt` is
  imported from the game folders, and updates, DLC and games can be installed into the emulated Wii U storage from a
  folder (Settings → Install updates and DLC).
- **Graphic packs.** The community packs download from the app and are enabled per game with their presets (resolution,
  anisotropic filtering, FPS, enhancements). Mods and translations that replace game files work as graphic packs too
  ([how](https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/docs/XBOX_TESTERS.md#mods-and-translations)).
- **Players 1-4.** Controller pairing per player (player 1 is the GamePad, 2-4 are Pro Controllers), a Nintendo button
  layout preset (A↔B, X↔Y) with 3 profiles per player, single-button remapping, per-stick deadzone up to 90 % with a live
  drift readout, a mappable "blow into the mic", and controllers that reconnect after a disconnect. On a PC the keyboard
  plays as player 1.
- **Motion controls.** Tilt the GamePad with the right stick (in-game menu → Motion), or use a phone's real gyro over a
  DSU / cemuhook app on the same Wi-Fi.
- **In-game menu** (click both sticks, or hold View + Menu; a hint shows when a game starts): switch between the TV and
  GamePad screens, a touch cursor for GamePad-touch games, FPS counter, VSync, volume and player 1's stick deadzone
  (applied right away), motion on/off, **amiibo** (dumps in `LocalState/amiibo`), a **Skylanders portal** (figures in
  `LocalState/skylanders`), and exit to the game list.
- **Wii U dialogs with a controller.** The on-screen keyboard and error dialogs can be used with the controller.
- **Settings.** Language and region (the console language games use for text and voices, console region, and the
  launcher's own language: English, Português (Brasil), Español, extendable with your own translation files), audio with
  5.1 surround, the shader and pipeline options below, VSync and an FPS counter.
- **Logs and backups.** Copies of the logs and your saves to the USB drive; after a crash the next start shows a notice
  and the crash is in `log_previous.txt`; a note when a newer build is available.

#### The Direct3D 12 renderer
- A complete D3D12 backend next to Cemu's Vulkan and OpenGL ones (Xbox apps can only use D3D12). Shaders are translated
  with SPIRV-Cross and compiled with DXC (default) or FXC.
- **Stutter control.** Shaders are compiled asynchronously and, in "Smooth" mode, never on the render thread: a new
  effect pops in a frame late instead of freezing the game. Pipelines are created in the background; each level's
  pipelines are remembered and created on its loading screen the next time it loads; "Quick start" creates only the
  startup pipelines on the loading screen. Pipelines a game hasn't used for 20 sessions, and pipelines that crashed the
  driver before, are skipped.
- **Memory management for the Xbox's 5 GB app budget.** Unused textures are released under memory pressure, large
  uploads are split and submitted early, a texture allocation that runs out of memory is retried after freeing cold
  textures, fewer pipelines are compiled at once near the limit, and the process heap is compacted under pressure.
- **Stability.** A GPU watchdog logs stalls, a command list that fails is dropped instead of closing the app, and after
  a GPU reset the app asks for a restart instead of failing silently. Device-removed diagnostics (DRED) and the D3D12
  debug layer's messages go to the log.
- **Frame interpolation** (60 to 120 FPS) for PCs with a 120 Hz display. It pauses itself under 100 Hz, so it has no
  effect on the Xbox, which shows UWP apps at 60 Hz.
- Diagnostics for bug reports: texture memory breakdown, format support, array texture views, optional texture dumps.

#### Windows builds
- `cemu-d3d12-windows-x64.zip`: regular Cemu (the wxWidgets interface) with the Direct3D 12 renderer selectable in the
  graphics settings.
- `cemu-host-win32-x64.zip`: the Xbox frontend as a Windows program, for trying the launcher and the controller features
  on a PC.

#### Mario Kart 8 first person camera (included)
This build **comes with** a first person camera pack for Mario Kart 8 (update v4.2): enable *Mario Kart 8 → Mods → First
Person Camera* in the graphic pack list. In a race, click the **right stick** to switch between a cockpit view with Mario's
hands on the wheel, first person without the kart, and the normal camera. The pack's options choose whether the camera
stays steady or spins with the kart during tricks, and how much of the arms to show. It also works in other Cemu builds:
unzip `MK8_FirstPerson.zip` into their `graphicPacks` folder.

### Status
Mainly tested with Mario Kart 8 (including 4 player split screen and the DLC). Other games may work; reports are welcome
in the [issues](https://github.com/PrismFox67/Cemu-UWP/issues) (there's a bug report form that asks for the logs). Known
limits on the Xbox:
- The first launch of a game compiles every shader once and takes a few minutes; later launches spend about a minute
  rebuilding pipelines, because the Dev Mode driver can't save them between launches (Quick start shortens it).
- The biggest modded tracks can run out of memory at 4K; use 1440p.
- Online play isn't supported.

Technical details: [D3D12 backend README](https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/src/Cafe/HW/Latte/Renderer/D3D12/README.md) ·
[UWP testing notes](https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/UWP_TESTING.md) ·
[tester guide](https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/docs/XBOX_TESTERS.md) ·
[compatibility list](https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/docs/COMPATIBILITY.md)

---

# **Cemu - Wii U emulator**

*The original Cemu README follows.*

[![Build Process](https://github.com/cemu-project/Cemu/actions/workflows/build.yml/badge.svg)](https://github.com/cemu-project/Cemu/actions/workflows/build.yml)
[![Discord](https://img.shields.io/discord/286429969104764928?label=Cemu&logo=discord&logoColor=FFFFFF)](https://discord.gg/5psYsup)
[![Matrix Server](https://img.shields.io/matrix/cemu:cemu.info?server_fqdn=matrix.cemu.info&label=cemu:cemu.info&logo=matrix&logoColor=FFFFFF)](https://matrix.to/#/#cemu:cemu.info)

This is the code repository of Cemu, a Wii U emulator that is able to run most Wii U games and homebrew in a playable state.
It's written in C/C++ and is being actively developed with new features and fixes.

Cemu is currently only available for 64-bit Windows, Linux & macOS devices.

### Links:
 - [Open Source Announcement](https://www.reddit.com/r/cemu/comments/wwa22c/cemu_20_announcement_linux_builds_opensource_and/)
 - [Official Website](https://cemu.info)
 - [Compatibility List/Wiki](https://wiki.cemu.info/wiki/Main_Page)
 - [Official Subreddit](https://reddit.com/r/Cemu)
 - [Official Discord](https://discord.gg/5psYsup)
 - [Official Matrix Server](https://matrix.to/#/#cemu:cemu.info)
 - [Setup Guide](https://cemu.cfw.guide)

#### Other relevant repositories:
 - [Cemu-Language](https://github.com/cemu-project/Cemu-Language)
 - [Cemu's Community Graphic Packs](https://github.com/cemu-project/cemu_graphic_packs)

## Download

You can download the latest Cemu releases for Windows, Linux and Mac from the [GitHub Releases](https://github.com/cemu-project/Cemu/releases/). For Linux you can also find Cemu on [Flathub](https://flathub.org/apps/info.cemu.Cemu).

On Windows, Cemu is available both as an installer and in a portable format, where no installation is required besides extracting it in a safe place.

The native macOS build is currently purely experimental and should not be considered stable or ready for issue-free gameplay. There are also known issues with degraded performance due to the use of MoltenVK and Rosetta for ARM Macs. We appreciate your patience while we improve Cemu for macOS.

Pre-2.0 releases can be found on Cemu's [changelog page](https://cemu.info/changelog.html).

## Build Instructions

To compile Cemu yourself on Windows, Linux or macOS, view [BUILD.md](/BUILD.md).

## Issues

Issues with the emulator should be filed using [GitHub Issues](https://github.com/cemu-project/Cemu/issues).  
The old bug tracker can be found at [bugs.cemu.info](https://bugs.cemu.info) and still contains relevant issues and feature suggestions.

## Contributing

If you want to contribute you can take a look at our [contribution guidelines](/CONTRIBUTING.md).

## License
Cemu is licensed under [Mozilla Public License 2.0](/LICENSE.txt). Exempt from this are all files in the dependencies directory for which the licenses of the original code apply as well as some individual files in the src folder, as specified in those file headers respectively.

## Credits (UWP / Xbox port)
- **Cemu UWP** by [PrismFox67](https://github.com/PrismFox67/Cemu-UWP)
- **Cemu** and the Cemu logo by the [Cemu team](https://cemu.info)
- The title installer (`src/gui/host/HostTitleInstaller.*`), the texture allocation recovery in the D3D12 renderer and the flip pacing experiment are adapted from [danprice142/Cemu-UWP](https://github.com/danprice142/Cemu-UWP) (MPL-2.0); the adapted code is marked in the files
- Box art from [GameTDB](https://www.gametdb.com), downloaded for the games in your game folders
- Menu sounds: [Kenney](https://kenney.nl/assets/interface-sounds) Interface Sounds (CC0)
- Launcher design inspired by SternXD's Dolphin UWP
- Menu music decoders: stb_vorbis (Sean Barrett) and dr_mp3 / dr_wav / dr_flac (David Reid), public domain
