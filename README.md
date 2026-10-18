# Tearaway Unfolded for Windows

**Tearaway Unfolded's PS4 game code running natively on Windows, with the graphics translated to Vulkan.**
It is built on the native Bloodborne port.

> **Early bring-up.** The game boots into its first chapter, but nobody has played through it yet.
> Expect missing graphics and other bugs. See [Status](#status), and [Getting started](#getting-started)
> to build and run it.

> **No game files are included.** You need your own decrypted dump of Tearaway Unfolded: the folder with
> `eboot.bin`, `sce_sys/` and `sce_module/` (developed against the US version 1.00, CUSA01607).
> This project is not affiliated with Sony Interactive Entertainment or Media Molecule.

## Status

The game gets through startup and runs its main loop for minutes without hitting an unsupported import:
about 60 frames a second, audio output, controller reads and the save mounted.

**The game boots into its first chapter.** After the "Unfolding" loading screen it asks you to sign
in to PSN (answer **No**), plays the intro video and streams *Valleyfold – Standing Stones*. In a
five-minute test it held about 60 fps with no crash. Nobody has played it yet, and the picture
has not been checked.

- **Fixed: the loading hang.** The game learns that its loading thread has finished by calling
  `scePthreadGetprio` on it, and waits for an error. On a PS4, an exited thread that isn't joined yet
  makes that call fail with `ESRCH`. Here it succeeded, so the loader stayed in state 1 forever.
  `scePthreadGetprio`/`Setprio` now return `ESRCH` for exited threads.
- **Fixed: the sign-in dialog loop.** The game says "You must sign in to the PlayStation™Network…"
  (Yes/No). The port used to answer Yes at once, so the game retried sign-in in a loop. Message
  dialogs now open as a Windows message box and wait for your answer. Choose **No** to play offline.
  `BB_MSG_DIALOG=yes|no` answers without a box.
- **Fixed: the GPU device loss after the dialog.** From the camera-less intro on, the game issues
  draws whose resource-table pointer is null or stale. A console never reads through those tables,
  but the emulator walks them all up front. Upstream's fault handler then rewrote the walker code
  to read zero forever, so every later draw of that shader got garbage descriptors. The result was
  thousands of junk shader permutations, the walker code space filling up, video memory running
  out and finally a device loss. Now:
  - A faulting walk ends at its first bad read, through bbport's recovery point (`WalkSrt`),
    and the walker code stays intact.
  - A draw or dispatch whose walk faulted is skipped before any permutation is compiled.
  - Repeats are remembered for 2 s, so each one costs a single exception.
  - Implausible texture descriptors bind as null (`PlausibleImage`): reserved formats,
    impossible tiling, or ranges outside mapped memory.
  - If you ran an earlier build, delete `user/cache/CUSA01607-nan1`. It holds the junk
    permutations.
- **Also fixed along the way:**
  - A silent microphone. `sceAudioIn*` now open and deliver silence at the real rate, where they used
    to fail.
  - MsgDialog result and close.
- **Tools for continuing:**
  - `BB_TRACE_IMPORTS=1` prints per-import call counts every 2 s.
  - `BB_TRACE_FILES=1` logs opens and directory listings. `=2` logs every file call.
  - The thread log shows each thread's entry point and exit.
  - `BB_GUEST_HOOKS` counts calls to game functions.
- A save cut off partway, for example by an earlier crash, also stops the game. Delete
  `user/savedata/1/CUSA00562` to start over.

Not known yet:

- **What it renders.** Rendering bugs specific to the game are the most likely next blocker.
- **Input.** Getting past the title screen, and the PS4 mechanics: the touchpad, the gyro and the microphone.
- **Later imports.** These still have no implementation and stop the game if it calls them:
  - the web browser, error and commerce dialogs
  - add-on content info and unmount
  - `sceSaveDataGetMountInfo`
  - one libkernel function with no known name (`-quPa4SEJUw`)

The game does not use sceNgs2 (Sony's audio mixer), so no mixer work is needed.

### What was added to boot it

- **Game modules.** `libSceFace.prx` and `libSceHand.prx` are linked from the game's `sce_module/`.
- **Kernel.**
  - Event flags (`sceKernel*EventFlag`, in `src/runtime_sema.c`, tested by `tests/test_evf.c`). The game's job system uses them.
  - `sceKernelGetModuleInfo` (main module only).
  - `sceKernelFutimes` and `sceKernelGetdents`.
- **Services.**
  - `sceSystemServiceGetDisplaySafeAreaInfo` returns the whole screen.
  - `DisableMusicPlayer`, `scePlayGoClose`/`Terminate` and `sceContentExportInit`/`Term` do nothing.
  - `sceAppContentTemporaryDataMount2` returns `/temp0`.
- **Absent hardware.**
  - The microphone and the PlayStation Camera report that they are not connected.
  - The PS App companion server starts, but no phone ever connects to it.

### Next steps

1. Look at what it renders, starting with the paper and shadow shaders.
2. Find the upscaler shader hashes (`docs/upscaler.md`). Until then, the upscalers run on the final image.
3. Work out input for the touchpad, gyro and microphone mechanics.
4. Add a launcher and a packaged release. Until then, use `run.py`.

## Getting started

### What you need

- Windows 10 (1903 or later) or Windows 11, 64-bit.
- An Intel Core 4th generation (Haswell) or newer, an AMD Ryzen, or an AMD FX. The game uses instructions
  of the PS4's processor (AVX, BMI1, MOVBE, LZCNT).
- A graphics card with Vulkan 1.3 and a current driver, ideally with 6 GB of video memory or more.
- Your own **decrypted** dump of Tearaway Unfolded: a folder with `eboot.bin`, `sce_sys\` and
  `sce_module\`. Only the US version 1.00 (CUSA01607) has been tested.
- [MSYS2](https://www.msys2.org/), to build the game. The built game also uses its DLLs, so keep it
  installed.
- [Python 3](https://www.python.org/downloads/), to start the game.

### 1. Install the build tools

Install MSYS2 to `C:\msys64`. Open the **MSYS2 CLANG64** shell from the Start menu and install the
compiler and libraries:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross} \
  mingw-w64-clang-x86_64-{protobuf,openssl,zlib,miniupnpc,nlohmann-json}
```

### 2. Get the code and build it

Still in the CLANG64 shell, clone the repository with its submodules and build it:

```bash
git clone --recursive <this repository's URL> tearaway_pc
cd tearaway_pc
bash build.sh
```

This takes a few minutes the first time and produces `out\bb-probe.exe`. If you cloned without
`--recursive`, `build.sh` fetches the submodules itself. After pulling new changes, run
`bash build.sh` again.

### 3. Run the game

Open **PowerShell** in the `tearaway_pc` folder, point `BB_GAME_DIR` at your dump and start it:

```powershell
$env:BB_GAME_DIR = "D:\Games\CUSA01607"; python run.py
```

From Git Bash or the CLANG64 shell, the same thing is
`BB_GAME_DIR="D:/Games/CUSA01607" python run.py`.

What to expect:

- **The first start stutters.** Shaders compile the first time each one is needed, and they're
  cached for later runs.
- **A sign-in message appears:** "You must sign in to the PlayStation™Network…". Click **No** to play
  offline. Yes retries signing in, which can't work here, and asks again.
- **Fullscreen.** The game starts fullscreen at your desktop's resolution. Alt+Enter or F11 switches
  to a window, and `BB_FULLSCREEN=0` starts it in one. The picture keeps the PS4's 16:9 shape, so
  ultrawide screens show black bars at the sides.
- **Saves and caches** go to `user\`, beside `run.py`. Delete `user\cache` if a newer build behaves
  oddly with an old shader cache.

To keep a log for a bug report, add `2>&1 | Tee-Object play.log` to the PowerShell command.

### Controls

A **DualShock 4 or DualSense** works as on the PS4, including its touchpad and motion sensors. Other
controllers work too, without the touchpad. Keyboard, mouse and controller can be used together.

Keyboard and mouse defaults:

| Action | Key |
|---|---|
| Move (left stick) | W A S D |
| Camera (right stick) | Mouse, or I J K L |
| Cross / Circle / Square / Triangle | Space / C / F / E |
| L1 / R1 | Q / G |
| L2 / R2 | Right mouse button / Left mouse button |
| L3 / R3 | Left Shift / V |
| D-pad | Arrow keys (mouse wheel: left/right) |
| Options | Escape or Enter |
| Touchpad click | Tab |
| Touchpad swipe or drag | Hold the middle mouse button and move the mouse |

To change them, create `input.ini` beside `run.py`. Each line is `key.<action>=<key>`, for example
`key.touch=Mouse Right`; the full list of actions and key names is at the top of `src/runtime_pad.c`.
These settings are specific to Tearaway:

| Setting | Default | Meaning |
|---|---|---|
| `key.touch` | `Mouse Middle` | While held, the mouse drags a finger across the touchpad. Each drag starts in the touchpad's centre |
| `touch_sensitivity` | `1.0` | Touchpad pixels moved per mouse pixel |
| `motion` | `1` | Use the controller's gyro and accelerometer (DS4, DualSense, Switch Pro, via SDL3) |
| `gyro_sensitivity` | `1.0` | Scales the angular velocity |

## Contributing: the bring-up loop

1. List the PS4 imports the runtime doesn't provide:
   ```bash
   python tools/missing.py "D:\path\to\Tearaway"
   ```
   Not every missing import is a blocker. An import only stops the game when it is actually called.
2. Run the game:
   ```bash
   BB_GAME_DIR="D:\path\to\Tearaway" python run.py
   ```
   The runtime prints `STOP: first unsupported PS4 import` at the first one it can't handle.
3. Implement or stub that import:
   - For a runtime function, add an entry to the `exports[]` table of the right `src/runtime_*.c`.
   - Register its name with `python tools/nid.py libSceXxx sceXxxFoo`. This computes the NID, so no aerolib
     is needed.
   - A `.prx` the game ships in `sce_module/` goes in `DEFAULT_MODULES` in `scripts/link_modules.py`.
4. Repeat.

## Credits

Built on the Windows work of [SuperMedo](https://github.com/Supermedo) (Mohammed Albarghouthi)
and on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Linux port of Bloodborne by
deadinside28, and on the [shadPS4](https://github.com/shadps4-emu/shadPS4) renderer. Video decoding
(Videodec2) and the DLSS bridge come from
[IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR).

Also used: [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK (FSR),
FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX), the
[NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (`nvngx_dlss.dll` under NVIDIA's license),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [SDL3](https://github.com/libsdl-org/SDL),
[FFmpeg](https://ffmpeg.org), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[MSYS2](https://www.msys2.org) / [LLVM](https://llvm.org), [PyInstaller](https://pyinstaller.org)
and [Pillow](https://python-pillow.org).

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. No official art is included.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses.
