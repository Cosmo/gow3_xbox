# Xbox UWP port

Target: Xbox Series X Developer Mode. The same development package runs on Windows PC
for smoke testing. Initial priority is playable behavior; resolution and frame rate will
follow measurements. Commit at meaningful working milestones, not individual edits.

## Milestone 1 checkpoint: native loader

The package builds the existing `src/probe.c` loader and CPU runtime with clang-cl,
the Store CRT, WindowsApp.lib and UCRT winpthread. A small CoreApplication host supplies
activation, private configuration, logs and a 30-second initialization watchdog.
Windows desktop and Linux builds retain their existing implementations.

The package is **not playable yet**. Graphics, GPU event queues, audio, video decoding
and the guest controller APIs remain unresolved and are reported explicitly. Host
controller enumeration is diagnostic only. No successful guest implementation is
substituted for an unported service.

Validated on Windows PC and Series X on 2026-10-14:

- The UWP package registers/installs and activates.
- Package imports are available through WindowsApp.lib, the Store CRT or packaged DLLs.
- Allocating read-write memory, changing it to execute-read, and executing a System V
  x64 function returns the expected value, 42, on both machines.
- The EU dump's linked image maps 31,082,556 bytes in six segments. The existing loader
  applies 15,940 relocations and redirects 117 TCB loads to the Windows TLS slot.
- The actual game-supplied libc initializer starts and registers its application heap API.
- The desktop guest-memory allocator then stops with error 487 while reserving the
  entire PS4 user interval. UWP host allocations already occupy part of that interval.
- Series X reports a 5,120 MiB app-memory budget.
- The configured external dump is readable from PC UWP. It is not yet readable at the
  configured Xbox location; console evidence here covers packaged native code only.
- Deployment, private run configuration upload, launch and both log downloads work
  through Device Portal. No other console packages are changed.

The loader's early whole-range reservation is deferred in the UWP build so code loading
can be tested independently. The first guest pool allocation still uses the existing
allocator and produces the failure above. This is a diagnostic milestone, not a memory
workaround. The external dump-access check is separate from packaged-code execution:
an inaccessible dump does not establish working game-file IO.

## Files and privacy

- `platform/`: UWP host and compatibility adapters. Only this build uses the adapters.
- `scripts/`: setup, build, import verification, packaging, PC smoke tests and console operations.
- `.local/`: ignored device configuration, dependency copies, prepared game code, build output,
  signed packages, PC logs, transfer receipts and raw Xbox results.
- `settings.example.json`: generic local-settings schema. Actual paths stay in `.local/settings.json`.

Development packages contain private, dump-derived prepared executable code. They are
local test artifacts, not redistributable releases. Neither prepared code nor full game
assets are committed. Signing keys remain in the local Windows certificate store.

XboxResearch is a read-only reference. The console operations are adapted from its shared
scripts; this project's copies keep transfers and results under `.local/`. Dependency
copies include winpthread's license. Build and console operations do not write into the
reference project and do not require its scripts at runtime.

## Milestone 2: original game entry and sparse memory

The UWP allocator now reserves guest windows on demand and commits touched 16 KiB guest
pages. Host protection remains granular to 4 KiB, as required by GPU page tracking.
Direct-page contents survive unmapping/remapping. Live aliases promote just those physical
pages to small shared sections, retaining real alias coherence without one giant section.
Released pages are zero on reuse; partial flexible unmapping retains the remaining page.

All memory contract tests pass on PC UWP and Series X, including protected host copies
and mixed 4 KiB protections applied before the first touch. Both platforms complete the
linked libc and Fios initializers and execute the game's actual entry point. The game
maps 2.75 GiB of direct memory; at the current stop it has committed 107 guest pages,
about 1.67 MiB, in addition to loader/host memory and bookkeeping.

Current stop: `sceVideoOutOpen`, because the graphics backend is not yet linked into UWP.
The console can list the configured USB game folder and open its executable through both
native FromApp and WinRT APIs. Listing the drive root is unnecessary and remains denied.
`storage` mode captures this distinction; `memory` mode runs the memory contract tests.

Limits still to validate during rendering: memory pressure, large alias sets (the console
limits shared sections), and concurrency between GPU watchers and host copies. Private
protected host copies briefly open their pages while holding the runtime metadata lock;
guest writes during that window need the renderer's coherence policy to be checked.

## Reproduce locally

PowerShell 7.2+, Visual Studio C++ tools with clang-cl, a Windows SDK, CMake 3.20+,
Ninja and Python are required. Existing setup is stored in ignored `.local/`.
On a fresh checkout, `scripts/setup.ps1` accepts explicit `ResearchRoot`, `PcDump`,
`XboxDump` and `Python` paths and copies only the required local dependency files and
selected console configuration. It refuses to overwrite an existing setup.

```powershell
./xbox/scripts/build.ps1 -Prepare
./xbox/scripts/check-imports.ps1
./xbox/scripts/assets.ps1
./xbox/scripts/pc-smoke.ps1 -Register -Mode ready
./xbox/scripts/pc-smoke.ps1 -Mode boot
./xbox/scripts/pc-smoke.ps1 -Mode memory
./xbox/scripts/package.ps1
```

`ready` runs the native-code smoke test; `imports` lists unresolved imports; `boot`
attempts linked-module initialization and then the game's entry point. The package's
LocalState `run.txt` selects a mode on line 1 and a dump folder on line 2. Logs are
`LocalState/loader.log` and `LocalState/errors.log`. They close when the worker finishes;
startup failures that terminate the process also close them.

Console scripts require explicit `-Console` and `-App` selection. Use the installed
PackageFullName when more than one version exists. Run installation, initial `ready`
launch, then configuration upload: the first launch initializes LocalState. Uploads
refuse existing files unless `-Overwrite` is supplied. Each retained download requires
`-Probe`, a unique `-RunId`, and `-RunPath`; results stay under `.local/results`.

## Next milestones

Graphics build: set `research_root` and `mesa_root` in ignored `.local/settings.json`,
fetch the pinned headers with `xbox/scripts/boost-headers.py`, then run
`build.ps1 -Graphics`. `pc-smoke.ps1 -Mode graphics` checks the packaged Dozen driver,
a Vulkan clear, exported D3D12 image, and native CoreWindow presentation. The same
test passed on Series X and its displayed blue frame was verified by screenshot.
The frame stays visible until the app closes. This is a host graphics test, not gameplay.
Native-resolution UWP currently excludes the optional temporal upscalers.
The renderer is now connected in `boot` mode. The latest integration checkpoint is below.

1. Completed: allocation on demand, alias/remap preservation, libc and module initialization,
   and original game entry on both PC UWP and Xbox. Continue validating memory during rendering.
2. Prefer the user's Dozen fork for preserving the Vulkan renderer, after a bounded
   compatibility check. It already runs basic jobs on Series X, but lacks buffer device
   address below shader model 6.6, push descriptors and robustness2. Mutable image views
   also have a measured console failure. The existing reference Vulkan-to-D3D12 layer
   supplies proven workarounds and remains an alternative if the Dozen work is too broad.
   Capability names alone are insufficient; implement and test their behavior.
3. Port guest controller input, audio/media and game-file IO; verify saves and lifecycle.
4. Reach gameplay, then fit measured CPU/GPU memory and frame times to the console budget.

Reference measurements and Kyty gameplay results are useful evidence, but do not prove
the same memory patterns, shader behavior or performance for God of War III.

## Milestone 4: connected renderer and intro initialization

The full guest graphics services, controller APIs, silent audio timing, ATRAC9, MP3 and
video decoders are linked into the UWP executable. PC and Series X runs complete libc
and Fios initialization, enter the game, hide its system splash screen and start the
intro loader, audio driver and streaming threads. Package API validation passes.
The current PC run renders the Sony and Santa Monica logos, brightness setup,
the opening movie and the 3D main menu. Scripted Cross input advances setup,
New Game and difficulty selection. Single-sample rendering fixes the menu's
striped depth corruption and is the UWP default (`GOW3_NO_MSAA=0` restores MSAA).
Series X passes the narrow-buffer detiler after the Dozen compiler fix, reaches
the opening movie, and has verified TV output for the logos and opening movie. Playable
gameplay verification remains pending. Audio output remains silent.

The host handles UWP `BackRequested`, matching Kyty's game page, so Xbox B can
remain the guest Circle button without invoking system back navigation. Physical
B-button verification is pending. Visibility, suspension and resume events are
logged; startup memory samples are bounded to the first two minutes.

UWP uses ordinary GPU buffers in place of sparse arenas. Shader constant-memory reads
use a logical page table and an array of ordinary buffer descriptors instead of Vulkan
buffer device addresses. Host push bindings allocate, update and bind normal descriptor
sets, with their pools owned by command buffers. Absent guest buffers and images select
shader permutations returning zero, backed by real non-null resources. These paths need
continued validation with actual game draws, memory pressure and alias coherence.

All optional upscaling and RCAS are disabled; the console's FSR1 shader failed during
startup. The native CoreWindow bridge handles presentation. The UWP page-fault handler
commits new pages before asking GPU tracking to resolve their protection: the opposite
order retried the game's first heap write indefinitely. WinRT gamepad input and vibration
are connected, but no controller was attached during these runs. Audio output is silent.
The optional ImGui overlay stays disabled when its Vulkan loader lacks required functions.

PC startup uses an ordered draw worker and waits for buffer initialization before
guest draws. Rectangles and quads expand through geometry shaders; compressed
textures use buffer uploads, and image views stay within their RGBA/BGRA family.
The Dozen fork removes unused SSBO declarations before descriptor remapping,
avoiding rejected root signatures from the unused clip-plane buffer.

Earlier PC MSAA runs reported sample-count, image-view and resource-state
mismatches. Continue validating later draws and console memory pressure.
Dozen's compute compiler must use the device's native 16-bit capability for both
its NIR bit-size mask and buffer-operation lowering; otherwise the console
rejects the 8-bit texture detiler. Shader dumps and logs remain private in `.local/` or the
app's LocalState. `graphics-debug.txt` accepts `GOW3_DUMP_SHADERS`, `DZN_DEBUG`,
`DZN_MAX_SHADER_MODEL`, `DZN_MAX_D3D12_OPTIONS` and `GOW3_UWP_CAPTURE`; it is optional
and never packaged. Capture saves the latest frame and a bounded 32-image contact
sheet, sampled every five seconds, under `LocalState/user/`.
Dozen's separate static CRT writes `LocalState/dozen.log`, while the app writes its own
loader and error logs. Logs can be downloaded while the app is running.

The manifest declares the actual dump extensions because removable-storage access is
filtered by file type. A native open of the exact executable succeeds on the latest
console run, while its WinRT broker lookup still reports access denied; permissions need
validation for the remaining assets. See [Microsoft's file-access documentation](https://learn.microsoft.com/en-us/windows/uwp/files/file-access-permissions).

`pc-smoke.ps1 -Isolated -Mode boot` registers a separate `Gow3.Uwp.Smoke` development
layout if the primary PC package is stuck in servicing. Subsequent runs use
`-PackageName Gow3.Uwp.Smoke`; the console retains its original package identity.
