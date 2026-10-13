# Cemu UWP: Cemu with Direct3D 12, for Xbox Series X|S and Windows

**This is an unofficial fork of [Cemu](https://github.com/cemu-project/Cemu).** It adds a native **Direct3D 12 renderer**
and an **Xbox app (UWP) frontend made for Dev Mode**, so Wii U games run on an Xbox Series X|S. It isn't affiliated with
the Cemu team; please report problems with this build here, not to them.

On a Series X it runs **Mario Kart 8 at 1440p or 4K, locked at 60 FPS**, with 4 local players.

### Download
Everything is on the **[ci-latest pre-release](https://github.com/PrismFox67/Cemu-UWP/releases/tag/ci-latest)**, rebuilt
on every change:

| File | What it is |
|---|---|
| `cemu-uwp-x64.zip` | **The Xbox app.** Install it through the Xbox Device Portal. Setup and recommended settings: **[Xbox tester guide](docs/XBOX_TESTERS.md)** |
| `cemu-d3d12-windows-x64.zip` | Regular Cemu for Windows with the Direct3D 12 renderer added |
| `cemu-host-win32-x64.zip` | The Xbox frontend as a Windows program, for testing it on a PC |
| `MK8_FirstPerson.zip` | The Mario Kart 8 first person pack on its own, for other Cemu builds (the downloads above already include it) |

### What this build adds
- **Direct3D 12 renderer** next to Cemu's Vulkan and OpenGL ones (Xbox apps can only use D3D12).
- **An Xbox frontend built for a controller:** game list, settings, graphic pack downloads and presets, players 1-4 with
  controller pairing, stick deadzones, 5.1 audio, and an in-game menu (click both sticks) with a touch cursor for GamePad
  games.
- **Stutter control:** shaders and pipelines are compiled in the background ("Smooth" mode never stops the game for a new
  shader), pipelines from earlier sessions are rebuilt while the game loads, and each level's pipelines are created on its
  own loading screen.
- **Memory management for the Xbox's 5 GB app budget:** unused textures are released under memory pressure and large
  uploads are split, so big tracks fit at high resolutions.
- **Frame interpolation** (60 to 120 FPS) for PCs with 120 Hz displays.

### Mario Kart 8 first person camera
This build **comes with** a first person camera pack for Mario Kart 8 (update v4.2): enable *Mario Kart 8 → Mods → First
Person Camera* in the graphic pack list. In a race, click the **right stick** to switch between a cockpit view with Mario's
hands on the wheel, first person without the kart, and the normal camera. The pack's options choose whether the camera
stays steady or spins with the kart during tricks, and how much of the arms to show. It also works in other Cemu builds:
unzip `MK8_FirstPerson.zip` into their `graphicPacks` folder.

### Status
Mainly tested with Mario Kart 8 (including 4 player split screen and the DLC). Other games may work; reports are welcome
in the [issues](https://github.com/PrismFox67/Cemu-UWP/issues). Known limits on the Xbox:
- The Dev Mode driver can't save compiled pipelines, so they're rebuilt (in the background) after every launch.
- The biggest modded tracks can run out of memory at 4K; use 1440p.
- Frame interpolation has no effect on Xbox, which shows UWP apps at 60 Hz.

Technical details: [D3D12 backend README](src/Cafe/HW/Latte/Renderer/D3D12/README.md) · [UWP testing notes](UWP_TESTING.md)

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
