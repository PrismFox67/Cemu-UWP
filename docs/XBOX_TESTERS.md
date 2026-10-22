# Cemu D3D12 for Xbox Series X|S: tester guide

This is a fork of Cemu with a native Direct3D 12 renderer and an Xbox (UWP) frontend made for Dev Mode. On a Series X it runs
Mario Kart 8 at 1440p / 4K at a locked 60 FPS.

Download: [`cemu-uwp-x64.zip` from the ci-latest release](https://github.com/PrismFox67/Cemu-UWP/releases/tag/ci-latest)

## Install
1. Unzip on the PC. In the Xbox Device Portal, open **Add**, pick the `.msix` and add the `Dependencies/x64` package
   (Microsoft.VCLibs) as a dependency.
2. **Important:** in Dev Home, open the app's options (View button → View details) and set **App type: Game**. As an "App",
   it gets far less memory and only 1080p display modes. You can leave the second dropdown on "Retail".
3. Start it. Under **Settings → Game folders**, add the folder or USB drive with your games (decrypted dumps, .wua, or
   .wud/.wux disc images). Keep updates and DLC as their own folders next to the game (for example `MARIO KART 8 [Update]`
   and `MARIO KART 8 [DLC]`): Cemu finds them there, and the game list shows "(update)" and "+ DLC" when it does. Nothing
   needs to be installed.
4. Disc images (.wud/.wux) need their disc key. Put `keys.txt` in your game folder or at the root of the drive, then press
   **Settings → Import keys.txt from game folders**.
5. Games in `E:\Games\wiiu`, `E:\WiiU\games` or `D:\DevelopmentFiles\Games\wiiu` are found without adding the folder.
6. Updates and DLC that aren't next to the game can be installed into Cemu's mlc folder with **Settings → Install
   updates and DLC**.

## The launcher
- **LB / RB** switch between Games, Graphic packs, Controllers, Settings and About.
- **Games**: the covers scroll left and right. **A** starts the selected game, **X** refreshes the list, **Y** opens Settings.
  The line under the covers shows the game's region, title ID, version, and whether an update or DLC was found.
- **Settings → Look**: light or dark panel. For your own background, put `background.png` or `background.jpg` into
  `LocalState/theme` and restart Cemu.
- **Settings → Language and region**:
  - **Console language** is the Wii U system language that games use for their text and voices. It applies the next
    time a game starts.
  - **Console region** should stay on **Auto** (it follows the game).
  - **Interface language** sets the language of the launcher: English, Português (Brasil) or Español. By default it
    follows the console language. To translate more texts or another language, put lines of the form
    `English text<TAB>translation` into `LocalState/translations/<code>.txt` (for example `pt.txt`).
- **Settings → Logs and backups** copies the logs and your saves to the drive of your game folder (a USB drive).

## Recommended settings (Series X)
**Settings tab**
- **New shaders: Smooth.** Draws never wait for a new shader, so there's no stutter. A new effect can pop in a frame late.
- **Shader compiler: DXC** (the default). It starts games about twice as fast as FXC.
- **Compile shaders asynchronously:** on.
- **Compile pipelines while the game loads:** on.
- **VSync:** on. **Frame interpolation:** off (the Xbox shows UWP apps at 60 Hz, so it has no effect there).
- **Show FPS:** on, if you're filming.

**Graphic packs tab**: press **Download community graphic packs**, then for Mario Kart 8 → **Graphics**:
- **TV Resolution: 2560x1440 (2K).** This is the safest setting: the big tracks fit in memory. 3840x2160 (4K) looks great
  and runs at 60, but the largest tracks can still run out of memory on the Xbox.
- **Anisotropic Filtering: 16x.** It costs almost nothing.
- Leave the rest at default. Optional: Enhancements → Debanding, and Remove MKTV Watermark.

**Controllers tab**: controller 1 is the GamePad, controllers 2-4 are Pro Controllers.
- **Stick drift**: each player has a **Left/Right stick deadzone** (0-90 %). The line below shows how far each stick is
  off center right now; with the stick let go, pick a deadzone a bit above that number.
- **Nintendo button positions**: open **Button mapping / Nintendo layout** for the player and press
  **Nintendo layout (A<->B, X<->Y)**. **Reset to Xbox layout** goes back. Single buttons can be remapped there too.
- **Blow into the microphone** (Super Mario 3D World and others): under Button mapping for the GamePad player, map
  **Blow into mic (hold)** to a button you don't need (for example the right stick click); holding it blows.

**In-game menu**: click both sticks, or hold View + Menu for 1 s. It has: switch to the GamePad screen, a **touch cursor**
(right stick moves it, RT taps, for games like Super Mario Maker), the FPS counter, and exit to the game list.
**Motion controls** (gyro puzzles, e.g. BOTW shrines): in the in-game menu set **Motion: On**. The right stick then tilts
the GamePad (let go = flat). Turn it off again to get the camera back on the right stick. Real gyro: install a DSU/cemuhook motion app on
your phone (same Wi-Fi), type the IP it shows under **Controllers → Phone motion (DSU) IP** for the GamePad player and
press **Use phone motion**.

**amiibo**: upload amiibo dumps (`.bin`) to `LocalState/amiibo` with the Device Portal. In the in-game menu, pick one on
**Scan amiibo** with left/right on the D-pad and press A when the game asks for an amiibo.

**Skylanders**: upload figure dumps (`.sky`, `.bin` or `.dmp`, 1 KB each) to `LocalState/skylanders`, then start the game
(the emulated portal is only connected when that folder has figures at launch). In the in-game menu, pick a figure on
**Skylander** with left/right and press A to put it on the portal; A again takes it off. Picking another figure and
pressing A swaps it.

## Mods and translations
Mods that replace game files (translations, texture or model swaps) work as graphic packs. A pack is a folder with a
`rules.txt` and a `content` folder that mirrors the game's `content` folder; only the files you put there are replaced.

```
MyTranslation/
  rules.txt
  content/
    Message/...        (same path and name as the file in the game's content folder)
```

A minimal `rules.txt` (the title IDs are the game's, all regions you want it for, comma separated):

```
[Definition]
titleIds = 000500001010ec00,000500001010ed00,000500001010eb00
name = My translation
path = "Mario Kart 8/Mods/My translation"
description = Replaces the game text
version = 6
```

To add it: in the Device Portal, **File explorer** → **LocalAppData** → the **CemuUWP** package → **LocalState** →
**graphicPacks**, create the pack's folders and upload the files. Restart Cemu, then enable the pack under **Graphic packs**
for that game. It applies the next time the game starts.

## What to expect
- **First launch of a game is slow.** Every shader is compiled once, and the game starts after a few minutes on the loading
  screen. Expect some pop-in and short hitches the first time each track or effect appears. Each shader is compiled only once,
  so later runs don't have this.
- **Later launches take about a minute of "pipelines" loading** (MK8: about 55 s on a Series X). The Xbox driver can't keep
  compiled pipelines between launches, so they're rebuilt every time. If you'd rather start sooner, enable **Quick start**
  under the pipeline option: the menus and race start load first, the rest in the background. Pipelines a game hasn't
  used for 20 sessions are skipped.
- **In game:** a locked 60 FPS at 1440p or 4K in Mario Kart 8. Performance in other games varies, and reports are very welcome.
- **Known issues:** 4K on the largest tracks can still run out of memory and close the app (use 1440p). Online play isn't
  supported.

Tested games and their status: [COMPATIBILITY.md](COMPATIBILITY.md).

## Reporting problems
Open an issue with the **[Xbox / D3D12 bug report form](https://github.com/PrismFox67/Cemu-UWP/issues/new/choose)**.
Results from other games (working or not) are welcome too.

**Attach the logs.** In the Device Portal on a PC: **File explorer** → **LocalAppData** → the **CemuUWP** package →
**LocalState**, then download:
- `log.txt`: the current or last session. It can be read while the game runs.
- `log_previous.txt`: the session before. Starting Cemu moves the old log here, so **after a crash and a restart, the
  crash is in `log_previous.txt`.**

A crash adds a "Crashlog" section with the stack to the log. If the log just stops without one, the Xbox probably closed
the app for using too much memory; the "Host: memory" lines show how close it was.
