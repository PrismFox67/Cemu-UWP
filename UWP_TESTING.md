# Testing the D3D12 renderer and the UWP / Xbox build

This fork adds a Direct3D 12 renderer and a controller-friendly frontend that runs as a UWP app (Xbox Series X|S in
Developer Mode, or Windows). Nothing here is an official Cemu release. Test in this order, because each step narrows
down where a problem is:

| Step | Build | What it tests |
|---|---|---|
| 1 | `cemu-d3d12-windows-x64.zip` | The D3D12 renderer inside the normal Cemu (wxWidgets UI, all debugging tools) |
| 2 | `cemu-host-win32-x64.zip` | The Xbox frontend (launcher, in-game menu, XInput/keyboard) as a normal desktop program |
| 3 | `cemu-uwp-x64.zip` on Windows | The UWP packaging, sandbox and file access, still on a PC |
| 4 | `cemu-uwp-x64.zip` on Xbox | The real target |

All builds come from the fork's CI and are attached to the
[`ci-latest` pre-release](https://github.com/PrismFox67/Cemu-UWP/releases/tag/ci-latest). Build logs are in the
[`ci-logs` pre-release](https://github.com/PrismFox67/Cemu-UWP/releases/tag/ci-logs).

Use a decrypted game format (`.wua`, `.wuhb`, `.rpx`, or an extracted `code/content/meta` folder) or a `.wud/.wux`
with the matching key in `keys.txt`. Homebrew (`.rpx`/`.wuhb`) is the quickest first test because it starts without
a dumped system.

## 1. Normal Cemu with the D3D12 renderer (PC)

1. Unzip `cemu-d3d12-windows-x64.zip` to a new folder (it's portable; settings go next to the exe).
2. Start `Cemu.exe`, go to **Options → General settings → Graphics**, set **Graphics API** to **Direct3D 12** and
   pick your GPU. Restart the game if one was running.
3. Run a game. The first run of every game compiles shaders, so expect stutter (or brief glitches with
   **Async shader compile** on). They are cached afterwards.

What to report: crashes (with `log.txt`), wrong colours, missing geometry, black screen, and the FPS compared to
Vulkan on the same game. Two environment variables help:

- `CEMU_D3D12_DEBUG=1` turns on the D3D12 debug layer (needs **Graphics Tools** from Windows' *Optional features*).
  Validation messages are written to `log.txt`. Slow, only for hunting a specific bug.
- `CEMU_D3D12_SHADER_COMPILER=dxc` compiles shaders with DXC instead of FXC (`dxcompiler.dll`/`dxil.dll` are
  included). Try this if shader compilation is very slow or a shader fails to compile.

Set them in a command prompt before starting: `set CEMU_D3D12_DEBUG=1` then `Cemu.exe`.

## 2. Xbox frontend on the PC (`Cemu_Host.exe`)

1. Unzip `cemu-host-win32-x64.zip` to a new folder and start `Cemu_Host.exe`. User data (settings, `mlc01`,
   `keys.txt`, shader caches) goes to the `user` folder next to the exe.
2. Settings tab → **Add folder...** (or type a path and press **Add path**) → back to Games. Or use **Open file...**
   on the Games tab to start a file directly. You can also pass a game on the command line: `Cemu_Host.exe -g <path>`.
3. Controls:
   - Launcher: D-pad/left stick to move, A to select, B to go back, LB/RB to switch tabs, Y for settings.
     Mouse and keyboard work too.
   - In game: **click both sticks** (L3 + R3) or **hold View + Menu (Back + Start) for one second** to open the menu
     (Resume, Pair controllers, show TV or GamePad screen, FPS counter, Exit to game list, Quit Cemu). On a keyboard,
     F1 opens it, arrows and Enter select, Esc closes it. **Exit to game list** stops the game (progress the game
     didn't save is lost) and returns to the launcher; **Quit Cemu** closes the program.
   - Players: controller 1 (plus the keyboard) is the Wii U GamePad for player 1, controllers 2-4 are Pro Controllers
     for players 2-4. Turning on a second controller is enough for a second player.
   - **Controllers** tab in the launcher: pick the controller type (Wii U GamePad, Pro Controller, Classic
     Controller, not connected) and the physical controller for each player.
   - **Pair controllers** (Controllers tab or in-game menu): press A on the controller for player 1, then player 2,
     and so on, then Menu (Start) to finish. B / Esc cancels. In the launcher, players without a controller are
     disconnected; in game they keep their setup, because adding or removing controllers while a game runs reportedly
     can make the Xbox terminate the app. To change the number of players, pair from the launcher.
   - Keyboard (player 1): left stick W/A/S/D, D-pad arrow keys, A/B/X/Y = K/J/I/U, L/R = Q/E, ZL/ZR = Z/C,
     Plus = Enter, Minus = Backspace, Home = H, right stick on the number pad (8/4/2/6).
   - Mappings are stored as normal Cemu profiles in `user/controllerProfiles/controller<N>.xml`; edit them or copy
     some from the normal Cemu to change individual buttons.
   - Alt+Enter or F11: fullscreen.

## 3. UWP package on Windows

The zip contains `CemuBin_<version>_x64.msix` (the app), `CemuBin_<version>_x64.cer` (the test certificate it is
signed with, generated per CI run), `Dependencies\x64\Microsoft.VCLibs.x64.14.00.appx` and Visual Studio's install
scripts.

1. Unzip `cemu-uwp-x64.zip`.
2. Turn on **Settings → System → For developers → Developer Mode**.
3. Right-click `Install.ps1` → **Run with PowerShell**. It asks for administrator rights once to trust the
   certificate, then installs the dependency and the app.
   Manual alternative: double-click the `.cer` → **Install Certificate** → **Local Machine** → **Place all
   certificates in the following store** → **Trusted People**; then install the VCLibs `.appx` and the `.msix` by
   double-clicking them.
4. Start **Cemu UWP** from the Start menu.

Without a certificate (Developer Mode on): rename the `.msix` to `.zip`, extract it into a folder that stays, delete
`AppxSignature.p7x` from it, and run `Add-AppxPackage -Register <folder>\AppxManifest.xml` in PowerShell (install the
VCLibs `.appx` first if it is missing). Start a game directly with `Start-Process "cemu:?path=<url-encoded path>"`.

Files: the app's user data is in `%LOCALAPPDATA%\Packages\CemuUWP.Unofficial_<id>\LocalState` (put `keys.txt` there).
A UWP app can only read folders that it's allowed to. Either keep games inside `LocalState`, or give the game folder
the `ALL APPLICATION PACKAGES` permission as described in step 3 of the Xbox section below, then add its path under
Settings → Game folders.

## 4. Xbox Series X|S (Developer Mode)

### One time setup

1. Activate Developer Mode: install the **Xbox Dev Mode** app from the Store on the console, register a
   [Partner Center](https://partner.microsoft.com) app developer account, enter the activation code shown by the app
   at `partner.microsoft.com/xboxconfig/devices` and switch to Developer Mode
   ([Microsoft's instructions](https://learn.microsoft.com/windows/uwp/xbox-apps/devkit-activation)). Retail games
   don't run while the console is in Developer Mode; you can switch back and forth.
2. In **Dev Home**, open **Remote Access Settings**, enable the **Xbox Device Portal** and set a user name and
   password. Note the console's address shown in Dev Home.
3. Prepare a USB drive for games on a PC: format it **NTFS**, create a folder such as `Cemu\games`, copy games there,
   then right-click the folder → **Properties → Security → Edit → Add** → type `ALL APPLICATION PACKAGES` →
   **Check Names** → OK → tick **Full control** → OK. This lets sandboxed apps read the folder. (Without it, the app
   only sees its own `LocalState` folder.) This is also the fast option: the app then opens game files directly,
   while folders opened through a file picker go through Windows' storage broker, which another Xbox Cemu port found
   to be slow on the console.

### Install

1. On the PC, open `https://<console address>:11443` in a browser (accept the certificate warning, log in).
2. **Home → My games & apps → Add** → choose `CemuBin_<version>_x64.msix` from the zip → Next → add
   `Dependencies\x64\Microsoft.VCLibs.x64.14.00.appx` as a dependency → Start. The test certificate does not need to
   be installed on the console.
3. On the console, in Dev Home, highlight **Cemu UWP**, press the View button → **View details** → set **App type**
   to **Game**. As an *App* the system limits it to about 1 GB of RAM, a few shared CPU cores and part of the GPU,
   which is not enough for Wii U games
   ([resource limits](https://learn.microsoft.com/windows/uwp/xbox-apps/system-resource-allocation)).

### Use

1. Plug in the USB drive, start Cemu UWP, go to **Settings → Game folders**, type the folder's path, for example
   `E:\Cemu\games`, and press **Add path**. (The **Add folder...** picker works on Windows but on Xbox the typed path
   is the reliable way.)
2. Encrypted `.wud/.wux` dumps need `keys.txt` in the app's `LocalState`. Upload it with Device Portal's **File
   explorer** (`LocalAppData\CemuUWP.Unofficial_<id>\LocalState`). The same folder holds `log.txt`; download it
   after a crash.
3. Sound: **Settings → Audio → Sound output: 5.1 surround** (new installs default to it) makes games that support
   surround, like Mario Kart 8, mix 5.1. What reaches the TV or receiver depends on the Xbox's own setting (Settings
   → General → Volume & audio output); `log.txt` shows the "XAudio2: … channel output" the app got.
4. Graphic packs (higher resolution, e.g. 4K): **Graphic packs** tab → **Download community graphic packs**
   (downloads the latest release of github.com/cemu-project/cemu_graphic_packs into `LocalState/graphicPacks`). The
   tab lists the packs of the game selected under **Games**; tick one and pick its presets, for example Mario Kart 8's
   `Graphics` pack with resolution `3840x2160`. Changes apply at the next game start. Own packs can be uploaded to
   `LocalState/graphicPacks` with Device Portal.
5. In the launcher B goes back; in a game B belongs to the game (the system's back action is suppressed, so it
   doesn't send the app to the background). Click both sticks for the in-game menu → **Exit to game list** to stop
   the game, or **Quit Cemu** to close the app.

### If it doesn't start or crashes

- Dev Home shows the app as installed but it closes immediately: get `log.txt` from `LocalState` via Device Portal.
  If there's no log, try the same package on Windows (section 3) to see whether the problem is specific to the Xbox.
- Very slow or out of memory: check that **App type** is **Game**. Even as a Game the process budget is limited;
  another UWP Cemu port measured about 5 GB on a Series S. Note the "Commited mem" values in `log.txt` and the
  memory use shown in Device Portal when it happens.
- No games listed: the folder needs the `ALL APPLICATION PACKAGES` permission (re-apply it to sub-folders), and the
  drive must be NTFS.

## Known limitations

- The D3D12 renderer is new. Mario Kart 8 (v1, no update) runs on an Intel Iris Xe with the Win32 host build: title
  screen, menus, character select and a Grand Prix race render correctly at 60 FPS. Other games are untested. See the
  [renderer README](src/Cafe/HW/Latte/Renderer/D3D12/README.md#known-risks) for the parts most likely to break.
- Keep "Compile shaders asynchronously" on (default). Without it a slow driver shader compile freezes the game; on
  Intel GPUs that took minutes for some Mario Kart 8 pipelines.
- The first start of a game compiles its shaders on the loading screen, which can take minutes with FXC. Later starts
  load them from the cache in seconds. After the shaders, the loading screen also creates every pipeline the game used
  before ("Loading cached pipelines"); on a new device or after a driver update that takes a while once, later starts
  are fast. Turn it off under Settings → "Compile pipelines while the game loads" for shorter loading but more stutter.
- To skip the first-time shader compiling and most of the stutter on the Xbox, play on the PC first, then copy from the
  PC's `user/shaderCache/` to the Xbox's `LocalState/shaderCache/` (same sub folders):
  - `transferable/<titleId>_shaders.bin` and `transferable/<titleId>_d3d12pipelines.bin`: which shaders and pipelines
    the game uses
  - `precompiled/<titleId>_d3d12_fxc.bin`: the compiled shaders. Optional but worth it: without it the first start
    compiles every shader again (Mario Kart 8 in the UWP build: about 12 minutes with it missing, 25 seconds with it)

  Don't copy `driver/`, it only works with the GPU driver that created it. Tested with the UWP build on Windows.
- The UWP/Xbox frontend has no button remapping, online or account UI yet, and graphic packs can't be changed while
  a game runs. Settings files from the normal Cemu (`settings.xml`, `controllerProfiles`, `graphicPacks`) can be
  copied into the user folder.
- Mario Kart 8 on the Xbox Series X: button icons inside text (the A in "Press A to start", the controller icons in
  the player list) show parts of other icons. They render correctly on the PC.
- No Vulkan/OpenGL, cubeb, SDL controllers, Wiimotes or DirectInput in the UWP build; audio uses XAudio2 and
  controllers use XInput.
- The PowerPC recompiler needs memory that is executable. The UWP build first asks for read/write/execute memory
  and falls back to two mappings of the same memory (one writable, one executable) if the system refuses; `log.txt`
  says which one was used. If both fail, the log says "Unable to allocate executable memory".
- Each CI build is signed with a new test certificate. On Windows, install the new `.cer` before updating; if an
  update is refused, uninstall the old version first (that removes its `LocalState`, so back it up).
