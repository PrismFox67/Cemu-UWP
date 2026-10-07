# Testing the D3D12 renderer and the UWP / Xbox build

This fork adds a Direct3D 12 renderer and a controller-friendly frontend that runs as a UWP app (Xbox Series X|S in
Developer Mode, or Windows). Nothing here is an official Cemu release. Test in this order, because each step narrows
down where a problem is:

| Step | Build | What it tests |
|---|---|---|
| 1 | `cemu-d3d12-windows-x64.zip` | The D3D12 renderer inside the normal Cemu (wxWidgets UI, all debugging tools) |
| 2 | `cemu-host-win32-x64.zip` | The Xbox frontend (launcher, in-game menu, XInput) as a normal desktop program |
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
   - In game: **hold View + Menu (Back + Start) for one second** to open the menu (Resume, switch TV/GamePad
     screen, FPS counter, Save and exit). On a keyboard, F1 opens it.
   - The first XInput controller and the keyboard are the Wii U GamePad. Keyboard: left stick W/A/S/D, D-pad arrow
     keys, A/B/X/Y = K/J/I/U, L/R = Q/E, ZL/ZR = Z/C, Plus = Enter, Minus = Backspace, Home = H, right stick on the
     number pad (8/4/2/6). The mapping is created on first start and stored in
     `user/controllerProfiles/controller0.xml`; edit that file or copy one from the normal Cemu to change it.
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

Files: the app's user data is in `%LOCALAPPDATA%\Packages\CemuUWP.Unofficial_<id>\LocalState` (put `keys.txt` there).
A UWP app can only read folders that it's allowed to. Either keep games inside `LocalState`, or allow the app to
read a game folder (step 3 of the Xbox section below; on Windows, alternatively enable
**Settings → Privacy & security → File system** for Cemu UWP).

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
   only sees its own `LocalState` folder.)

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
3. The B button in the launcher goes back; the Xbox button and the system's back gesture don't close the app. Use the
   in-game menu → **Save and exit** to stop a game.

### If it doesn't start or crashes

- Dev Home shows the app as installed but it closes immediately: get `log.txt` from `LocalState` via Device Portal.
  If there's no log, try the same package on Windows (section 3) to see whether the problem is specific to the Xbox.
- Very slow or out of memory: check that **App type** is **Game**.
- No games listed: the folder needs the `ALL APPLICATION PACKAGES` permission (re-apply it to sub-folders), and the
  drive must be NTFS.

## Known limitations

- The D3D12 renderer is new and has not run real games before this test. See the
  [renderer README](src/Cafe/HW/Latte/Renderer/D3D12/README.md#known-risks) for the parts most likely to break.
- The UWP/Xbox frontend has no graphic pack, controller mapping, online or account UI yet. Settings files from the
  normal Cemu (`settings.xml`, `controllerProfiles`, `graphicPacks`) can be copied into the user folder.
- No Vulkan/OpenGL, cubeb, SDL controllers, Wiimotes or DirectInput in the UWP build; audio uses XAudio2 and
  controllers use XInput.
- The PowerPC recompiler needs memory that is executable. The UWP build first asks for read/write/execute memory
  and falls back to two mappings of the same memory (one writable, one executable) if the system refuses; `log.txt`
  says which one was used. If both fail, the log says "Unable to allocate executable memory".
- Each CI build is signed with a new test certificate. On Windows, install the new `.cer` before updating; if an
  update is refused, uninstall the old version first (that removes its `LocalState`, so back it up).
