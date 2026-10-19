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

## Stutter diagnostics and memory-release fix

An instrumented desktop UWP run reproduced a long opening pause with about
10.7 seconds spent checking whether file-read buffers belonged to guest mappings,
while actual host reads in that interval took only 4 milliseconds. Memory unmaps
held the shared region-table lock for up to 3.4 seconds each. No guest swap-in or
swap-out occurred during the reproduction.

`uwp_detach` searched the entire virtual-page table for a replacement physical-page
owner even when only the mapping being removed remained. Large releases therefore
repeated the same unsuccessful scan for every page. The reference count now skips
that search for the last mapping; live aliases retain replacement-owner lookup.
The memory contract includes canonical-owner removal and final-alias removal.

The fixed desktop opening run logged no unmap above the 40 ms threshold, compared
with 50 before the fix; the former 11–12 second pauses disappeared. The same fix
is installed on Series X as development package `1.0.26289.2202`. Its timed opening
test also logged no long unmaps or paging. During combat at approximately 4.0 GiB
app usage, host reads took only milliseconds per five-second interval, while
graphics command/host-copy completion waits remained. This isolates a real
release-time defect without claiming all remaining hitches or low FPS are fixed.
Profiling itself has overhead, so these traces are not a clean FPS benchmark.

Private `graphics-debug.txt` accepts `GOW3_FRAME_STATS=1` and `GOW3_IO_STATS=1`.
Frame diagnostics separate compilation, graphics synchronization and collection
waits. I/O diagnostics report requested reads, small-read counts, actual host-read
time, guest-buffer copies and mapping-lookup time in five-second intervals. Totals
can overlap between threads and are not a frame-time decomposition. Paging reports
also show zero activity when I/O profiling is enabled. Diagnostics are opt-in.

Game files already use normal Windows caching; the compatibility layer does not
split reads into fixed 4,095-byte pieces. This fix adds no read-ahead buffer or
extra cache. Remaining renderer/command-recorder waits require separate analysis;
desktop results do not establish the console's complete frame-time behavior.

## Gameplay performance profile

UWP defaults now select 720p at a 60 Hz engine rate, no game multisampling,
deferred/previous-frame GPU readbacks, the two-stage draw pipeline and threaded
Vulkan recording. Saved pipelines remain cached. Cache rebuilding happens before
gameplay rather than competing with it; where supported, saved pipeline binaries
continue to use the renderer's on-demand path. New graphics shaders/pipelines
compile asynchronously, which can briefly delay a newly encountered effect.

UWP limits runtime compilation to two compile workers, one snapshot worker,
16 queued compile jobs and two retained IR pools. Pipeline warm-up also uses at
most two workers. Desktop builds outside UWP retain their existing concurrency.
Frames queued defaults to two, with cached host stream buffers. Copy/preparation
workers stay disabled as in the established Xbox compatibility profile.

Private `graphics-debug.txt` can override `GOW3_BACKGROUND_WARMUP`,
`GOW3_PARALLEL_WARMUP`, `GOW3_PIPELINE_BINARIES`, `GOW3_FRAMES_AHEAD` and
`GOW3_STREAM_MEMORY`, alongside the existing async/readback/cache switches.
These are renderer settings; cached host stream memory is unrelated to Windows'
file cache. Normal test profiles disable frame captures and detailed frame/I/O
diagnostics after verification, and leave scripted controls neutral.

A bounded desktop UWP combat check loaded 336/336 saved pipelines before play
in 2.06 seconds, rendered combat/effects and exited at its deadline. Native async
compiler tests and both desktop/UWP warm-up inbox checks passed. This is a
compatibility check, not proof of an FPS increase: multi-second command-completion
waits with zero synchronous shader compilations still occurred. The reported
orb-spawn hitch is not established as fixed. A live console trace before this
change also showed repeated FPS drops around 4.1 GiB usage with unchanged swap
counters; active swapping did not explain those particular drops.

Series X package `1.0.26289.2220` verified the same profile: 2,100/2,100 saved
pipelines loaded before gameplay in 14.56 seconds with two workers. The bounded
opening/combat run peaked at 4,587 MiB in periodic measurements, below the
5,120 MiB limit, with no recorded swap activity. Roughly three-second graphics
command-completion stalls remained. Both test apps were closed; console shutdown
initially timed out, then a subsequent process query confirmed no selected
package process. Normal configuration was verified by hash and scripted input
was empty. These results validate the profile without attributing the orb hitch
to a specific effect or claiming a measured FPS gain.

## Orb/effect stalls: Dozen pipeline state

Desktop command tracing isolated repeated 100–170 ms CPU stalls inside graphics
draw recording, rather than guest memory copies. Dozen emulates Vulkan dynamic
depth/stencil, rasterization and restart state with D3D12 PSO variants, created
at draw time. A cached Vulkan pipeline therefore did not mean its D3D12 variant
was ready, and the ordinary shader-compile timer missed this work.

On Dozen, effective PSO state is now part of `GraphicsPipelineKey` and supplied
as static Vulkan state. Async creation and cache warm-up can build the actual
D3D12 PSO before drawing. Viewports, scissors, stencil reference and other states
that do not require a new PSO remain dynamic. Other Vulkan drivers keep their
existing dynamic-state path. Primitive restart can still need Dozen's
index-format variants. `GOW3_BAKED_PIPELINES=0` restores the dynamic path for
comparison; it requires restarting the app.

Pipeline record version 6 ignores the old records, which lacked the required
state; shader/precompiled-template caches remain available. New records rebuild
as effects are encountered and load on later starts. A desktop restart loaded
278 new records in 10.59 seconds; stale old records were skipped.

Equal-duration, scripted desktop opening/combat traces with command diagnostics:

| Measurement | Before | First fixed run |
| --- | ---: | ---: |
| Draw calls taking over 10 ms | 248 | 3 |
| Longest draw call | 172.1 ms | 24.7 ms |
| Longest frame | 2,845.2 ms | 372.0 ms |
| Frames over 500 ms | 3 | 0 |
| Red-orb gain callbacks | 23 | 70 |

These are separate combat runs, not frame-identical FPS benchmarks. Remaining
shorter stalls are not claimed fixed, and this change has not yet been checked
on Xbox. Native checks cover disabled-state normalization, depth-clear write
suppression, independent/backface stencil masks, stencil-clear behavior, depth
bias scaling, emulated-primitive restart and states retained as dynamic.

A further restart/combat check with a simulated 5 GiB pager/cache budget recorded
92 red-orb gain callbacks. Paging completed 1,894 integrity checks with no
corruption (111 MiB written, 29 MiB read). This does not impose an OS memory cap
on desktop UWP. The user reported a concurrent PC build, so this final run's
frame timings are excluded from performance conclusions. The app was closed,
scripted input released, temporary memory-budget file removed and diagnostic
overrides cleared after testing. Xbox package `1.0.26289.2309` now contains this
fix (`d1f007e`). Installation, settings read-back and empty scripted input were
verified; the app was left closed for manual testing. Xbox gameplay validation
is still pending.

Private `GOW3_RECORDER_TIMINGS=1` logs recording commands taking at least 10 ms
and timestamps red-orb gain callbacks. It is disabled in normal use. Desktop
game tests use a stop watchdog and release scripted controls when finished.

### Arena checkpoint follow-up

The user still reports severe effect slowdowns on Xbox after the pipeline-state
fix. A copied Athena autosave now reproduces the later circular arena on PC;
the opening fight alone is insufficient validation. GPU timestamp profiling
stalled a diagnostic run at the menu and is disabled for this investigation.
CPU frame timers are refreshed after UWP reads private settings, before renderer
threads start, and `GOW3_PERF_DIAG` can enable the existing wait-site report.

Arena tracing found repeated 10–33 ms fence-recording callbacks. The UWP private
backing-write path queried all four host pages and changed/restored protection
across an entire 16 KiB guest page for even an eight-byte fence write. It now
queries only the touched 4 KiB pages and skips protection changes when already
writable. Restricted pages retain their original protection. The UWP memory
contract passes, including a write across mixed 4 KiB permissions, an untouched
no-access neighbor, aliases, and 8,242 swap integrity checks without corruption.
The memory contract also passes on Xbox; combat performance there remains
unverified. This is not a claim that all orb stalls are fixed. Test saves and
diagnostic configuration are restored after PC runs.

UWP region-table access now uses a native Windows SRW lock instead of the
winpthread reader/writer lock. Nested readers acquire the OS lock only once;
queries made while holding it exclusively keep the previous bypass behavior.
The memory contract also checks a nested query with a writer queued and a query
inside an exclusive operation. Opt-in frame diagnostics distinguish long fence
backing writes from interrupt notification; neither changes normal fence ordering.

Two 60-second scripted combat runs from the same Athena save, with the same input
and temporary survival/damage settings, isolated the table lock:

| Measurement | winpthread table lock | Native SRW table lock |
| --- | ---: | ---: |
| Fence backing writes taking at least 5 ms | 79 | 0 |
| Longest slow fence write | 33.01 ms | none at least 5 ms |
| Red-orb gain callbacks | 77 | 69 |

The fixed run still had individual long frames, so these results establish the
fence-stall improvement, not a smooth-gameplay guarantee or an overall FPS gain.
The PC app was closed after each test; temporary cheats, input, settings and saves
were restored. Xbox combat verification remains pending.

Xbox package `1.0.26290.18` contains the fix from `da3849a`. Installation and the
memory/concurrency contract passed on Series X, including the queued-writer
test and 8,242 swap integrity checks with zero corruption. A screenshot confirmed
the normal launcher opens. Graphics settings matched their pre-update hash,
boot mode was restored, scripted input was empty, and the app was verified stopped
after testing. This update does not change shader or pipeline cache formats.

## Files and privacy

- `platform/`: UWP host and compatibility adapters. Only this build uses the adapters.
- `scripts/`: setup, build, import verification, packaging, PC smoke tests and console operations.
- `.local/`: ignored device configuration, dependency copies, prepared game code, build output,
  signed packages, PC logs, transfer receipts and raw Xbox results.
- `settings.example.json`: generic local-settings schema. Actual paths stay in `.local/settings.json`.

Development packages contain private, dump-derived prepared executable code. They are
local test artifacts, not redistributable releases. Neither prepared code nor full game
assets are committed. Signing keys remain in the local Windows certificate store.

The build depends on no other project. `scripts/fetch-deps.py` downloads every third-party
library and tool at a pinned version from its upstream source, checks its SHA-256 and unpacks
it into `.local/deps` with its license; `scripts/build-deps.ps1` also builds glslang and a
static LGPL FFmpeg 7.1.1 limited to the codecs and demuxers the game uses (clang-cl through
MSYS2, which must be installed at `C:\msys64` or `-Msys2`). The console scripts keep transfers
and results under `.local/`.

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
Ninja, Python and MSYS2 are required. Existing setup is stored in ignored `.local/`.
On a fresh checkout, `scripts/setup.ps1 -PcDump <folder> [-XboxDump <folder>] [-Python <exe>]
[-MesaRoot <folder>]` writes `.local/settings.json`, creates `.local/devices.json` from
`devices.example.json` (set the console address there; credentials come from the
`XBOX_USERNAME`/`XBOX_PASSWORD` environment variables) and builds the dependencies. It
refuses to overwrite an existing setup.

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

Graphics build: set `mesa_root` (the Dozen fork's workspace) in ignored
`.local/settings.json`, then run `build.ps1 -Graphics`; it builds the dependencies first
when they are missing. `pc-smoke.ps1 -Mode graphics` checks the packaged Dozen driver,
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
gameplay is verified on desktop UWP: the user confirmed movement, attacks and sound
after the page-protection fix. The run continued beyond nine minutes with the
performance panel active. Scene textures still show substantial corruption.
Series X reached the opening scene on Gaia,
then failed a guest page commit with error 1455. Iteration has returned to desktop UWP.
XAudio2 sound and the controller overlay toggle are user-confirmed on desktop and
Series X package 1.0.26287.2315. Sustained console gameplay remains unverified.

The initial Kyty-style performance panel shows FPS, frame time history, process CPU
usage and total app RAM usage/budget. View + Menu + both triggers toggles it on
release, consuming the chord while held; F3 toggles it on desktop. The panel is
visible by default. FPS counts newly presented game frames, excluding reused frames.
GPU allocated/reserved rows report VMA live allocations and allocator blocks separately;
these are GPU figures, not values to add to the app's RAM total on every platform.

UWP uses 16 MiB preferred VMA blocks, following Kyty's GPU memory-savings path.
Staging prewarms 32 MiB, retains 64 MiB per ring and retires eligible idle blocks
after 120 ticks; in-flight GPU uses must still complete before release. The native
desktop renderer keeps its original defaults. Dense GPU arenas still retain their
allocations for the process lifetime, including superseded arenas: this needs a
separate safe retirement/eviction policy.

The packaged memory self-test measures app accounting around a 64 MiB guest-memory
allocation. On desktop UWP it reported 351 MiB before, 447 MiB after touching it and
383 MiB after releasing it: the release returned 64 MiB, and `VirtualQuery` confirmed
that those guest pages were no longer committed. This verifies the guest release path;
the later disk-paging implementation and its separate tests are described below.

Kyty's current Xbox code uses a cold-page pager (`src/xbox/lazyRegions.h`,
`pagerStore.h` and `src/kernel/memoryAddressSpace.inc`), rather than relying only on
the sparse-file approach described in older research notes. It uses 64 KiB windows,
atomic page restoration, host-I/O pinning, a dynamic guest budget, 768 MiB planning
headroom and a 384 MiB emergency margin. GoW's disk pager below adapts the archived
Kyty implementation while retaining GoW's physical aliases and GPU page protections.
Its lowered desktop test budget changes reclamation policy, not OS enforcement.

The host handles UWP `BackRequested`, matching Kyty's game page, so Xbox B can
remain the guest Circle button without invoking system back navigation. Physical
B-button verification is pending. Visibility, suspension and resume events are
logged; startup memory samples are bounded to the first two minutes.

Guest page commits, protected backing writes and GPU protection changes share the
same UWP lock. A fault on a committed page also restores its tracked protection
when the Windows protection disagrees. A desktop freeze was traced to a guest write
repeating with tracked read/write permissions while Windows kept the page read-only.
The packaged memory self-test covers concurrent commits/protection changes and
recovery from that mismatch. The fixed desktop run passed the frozen cinematic,
reached Gaia and responded to movement and attack input.

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
are connected. XAudio2 source voices preserve the runtime's steady buffer pacing and
stereo downmix; `GOW3_AUDIO=none` remains available for silent diagnostics.
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

`pc-smoke.ps1` closes either of this project's desktop test layouts before launching
another. Diagnostic modes close after collecting their logs; `boot` stays open for
gameplay. Close gameplay when a capture or input test is finished.

`pc-autostart.ps1 -Seconds 270` alternates A and X every half second in the Smoke package,
then releases scripted input. This passes the menus without manual input; the opening
cinematic still takes several minutes. It uses the existing private `controls.txt`.

`pc-smoke.ps1 -Mode texture` checks the actual detiler against a CPU Morton reference
at 8/16/32/64/128 bits, with three mips, two layers and a nonzero buffer offset.
It also checks padded texture uploads/readbacks for R8, RGBA8, BC1 and BC3. All nine
pass on PC UWP with the Dozen array-stride fix: `bufferImageHeight` must determine
the buffer offset of later array layers. These checks do not establish that the
later gameplay rendering is correct; that still needs visual comparison.

### Gameplay texture fix (15 October)

The UWP launcher now enables `GOW3_READBACK_LINEAR=1`, matching `run.py`.
The game reads GPU-written linear and small images while managing its textures.
Without these readbacks, valid compressed asset bytes become paired with incorrect
texture descriptors: Gaia shows rainbow stripes, foliage breaks up, and enemies
appear black. PC UWP gameplay captures with readbacks enabled show correct terrain,
foliage, and enemy textures. Disabling mipmaps and renderer caches did not repair it.

The build also prepares the existing native and 720p game patches from the game's
validated patch profile. `GOW3_UWP_RENDER_RES=720p` in private `graphics-debug.txt`
selects 1280x720 at the next launch. Native remains the default. Both profiles
include the video-arena correction and require a 7104 MiB virtual direct-memory
pool; physical memory is still committed on demand and remains subject to the
Xbox app budget.

`pc-autostart.ps1 -Capture -StopAfter` retains only the latest twelve five-second
captures and closes its own test process when the measurement ends. Optional
texture inspection compares guest memory, tiled GPU input, detiled output, and
image readback. Set `GOW3_UWP_TEXTURE_DUMP=1`; optionally restrict it with a
comma-separated `GOW3_UWP_TEXTURE_ADDRESSES` list. Dumps stay in private LocalState.

The Series X reached corrected native-resolution gameplay at about 23 FPS and
4335 MiB app RAM in an opening combat capture. A 720p run with deferred readbacks
showed about 27 FPS and 4103 MiB in the same opening area. These are scene samples,
not a controlled benchmark. The 720p frame capture confirms 1280x720 render targets.

`graphics-profile.example.txt` contains the tested private graphics switches; copy
it to the app's `LocalState/graphics-debug.txt`. The required linear-image readbacks
are always enabled by the UWP launcher. Persistent pipeline caching stores compiled
work between launches; asynchronous compilation remains an optional comparison
switch. `GOW3_UWP_METRICS=1` logs FPS, frame time, CPU, app RAM and GPU allocations
every five seconds, including when the performance panel is hidden.

The existing `user/graphics.cfg` accepts `vsync=0` for a comparison with unsynchronized
presentation. Preserve other keys when changing it. Keep startup patch selection
in `graphics-debug.txt`; the UWP app does not support the desktop restart command.

Some Xbox OS versions reject or stall on the package stop endpoint.
`xbox-stop.ps1` uses the process stop endpoint after checking each process still
belongs to the selected package, then verifies it is no longer running. Xbox may
retain an inactive process record; the result reports those IDs separately.

The final Series X comparison enables threaded Vulkan recording as well. Five-second
samples between 120 and 280 seconds averaged 25.0 FPS, versus 22.6 with direct recording;
these scripted opening-combat runs are not a controlled benchmark. App RAM stayed
between 4229 and 4279 MiB in that interval, with a 4500 MiB peak across startup and
gameplay, against the 5120 MiB budget. First-use shader compilation and effects still
cause frame-time spikes; this is not a locked 30 FPS build. The installed test package
is `1.0.26288.435`, using 720p, deferred/previous-frame readbacks, persistent caching,
threaded recording, and `vsync=0`. Background shader compilation and draw-preparation
workers remain disabled in the recommended Xbox profile.

### App memory pressure (15 October)

A later gameplay run failed in `UniqueImage::Create` with
`ErrorOutOfDeviceMemory`. Its last five-second sample was 4412 MiB app RAM and
1737 MiB GPU reserved, against a 5120 MiB app limit. The failed image's size was
not recorded, so these samples do not establish the allocation's peak demand.

UWP texture collection now uses `MemoryManager.AppMemoryUsage` and the current
app limit, including CPU guest pages and GPU memory together. Collection starts
768 MiB below the limit, increases pressure at 512 MiB headroom, and uses its
critical policy at 256 MiB headroom. Higher pressure considers younger cold
images. When eviction has occurred under pressure, the collector waits for the
GPU fence and retires deferred image destruction before another frame allocates.
GPU-written tiled images retain the existing protection against unsafe eviction;
required linear-image readbacks remain enabled. Skipped protected images move to
the back of the LRU queue so they cannot exhaust every bounded scan and starve
reclaimable images behind them.

For desktop stress testing only, private `graphics-debug.txt` accepts
`GOW3_GC_BUDGET_MB`. In UWP this lowers the collector's app budget without changing
the OS limit or the pager's budget. Values below 1024 MiB or above 65536 MiB are
ignored, and the value cannot raise the real app limit. Remove the override after
testing. A 4608 MiB stress run completed 236 seconds through opening combat with
correct textures; samples from 120 seconds onward used 4149-4202 MiB app RAM and
902-938 MiB GPU allocations. Startup still peaked at 4705 MiB: collection is not
a hard cap on live allocations.

Image allocation failures now log dimensions, format, layers, mip levels, sample
count, app memory, and VMA heap budgets to `loader.log`. The later crash scene
still requires validation; opening combat alone does not establish that the full
level fits in memory.

### Disk paging (15 October)

`platform/guest_swap.inc` adapts the Xbox memory design from KytyPS5 commit
`1303170`: a cached temporary LocalState file, memory-pressure headroom, pins for
guest-code accesses, and retention of GPU page protections. The reference source
was read from the archived Kyty repository; neither Kyty nor XboxResearch was
modified. Disk transfers use cached `ReadFile`/`WriteFile` calls at 64-bit offsets.
Page-in fills a small shared frame through a host alias before publishing guest
views, so concurrent readers cannot observe partially restored bytes.

Paging is enabled by default in gameplay. `LocalState/gow3-swap.bin` is a single
sparse temporary file, removed by Windows when the process closes. Its logical
length grows to the highest saved physical offset; only saved content requires
disk storage. The shared frame cache is at most 64 MiB, committed as used and
reused until process exit. Guest views use the individual 4 KiB GPU protections;
no-access subpages remain absent
until tracking permits access. Protected GPU readbacks update the file backing
without temporarily opening those guest pages. Remaps, physical reuse, and live
alias promotion preserve the memory contract. Guest file I/O uses a bounded
1 MiB host buffer: the kernel never accesses guest pages that the pager could
remove during a read or write.

Only large GPU-type direct mappings (type 3, at least 256 MiB) are candidates.
CPU heap mappings, stacks, live aliases, executable pages, and physical pages
touched by guest-code faults stay pinned. Late pins leave the shared cache for
their own resident section, preventing pinned pages from exhausting cache slots.
As in Kyty, repeated user-mode faults
in guest code can damage its System V red zone, so these pins are deliberate.
This is not an unlimited-memory guarantee: pinned guest memory and live GPU
resources still have to fit in the app budget. Later gameplay remains to be
validated.

The worker starts within 384 MiB of the app limit and attempts to recover 512 MiB
of headroom. It reclaims private pages, keeping a tracked residency bit instead
of repeatedly querying untouched pages. Already restored shared frames remain in the
bounded window until its own capacity needs room. Failed private commits can
reclaim memory synchronously; failed GPU image allocations can reclaim up to
four 64 MiB batches and retry before reporting failure. Logs report page-out,
page-in, stored content, mapped views, checksum checks, and pinned memory.

For pressure reproduction, `LocalState/memory_limit_mb.txt` accepts a budget from
2048 to 65536 MiB, clamped to the real OS limit. It changes the pager and cache
policies, not the OS enforcement or the overlay's actual OS limit. Delete the
file or write `0` after testing. Private `graphics-debug.txt` accepts
`GOW3_UWP_SWAP=0` to disable paging for a comparison.

`pc-smoke.ps1 -Mode memory` checks all 64 MiB of a patterned allocation at physical
offset 3 GiB with an 8 MiB shared cache, concurrent readers during eviction,
guest-code pins, mixed GPU protections, protected host writes, remaps, and physical
aliases. File I/O and late pins each span 16 MiB, exceeding the cache capacity.
The high-offset test catches an Xbox failure in the first implementation, where
mapping the disk file at a large offset returned error 8; disk file views have
been replaced with explicit transfers. The contract passes on desktop UWP and
Series X: the probe releases 48 MiB of private app RAM and performs 8,242 checksum
checks without corruption. The initial 4 GiB gameplay stress
run also exposed excessive background scanning and view churn, which were removed.

Both input helpers now alternate A (Cross) and X (Square), with a 200 ms hold
and a half-second target between presses. `xbox-autostart.ps1 -App <full-package>
-Console <name> -Seconds 210 -StopAfter` keeps twelve rotating five-second
screenshots in ignored `.local/transfers`. Network and screenshot time can
extend an Xbox input step. Both helpers release scripted input on exit; use
`-StopAfter` to close the measured game.

### Save-dialog input (15 October)

The UWP window now polls controller and scripted button edges for the host save
list independently of guest pad reads. Previously its event pump only slept:
gameplay input worked, but the SDL-based save list never received A/B or D-pad
events. CoreWindow forwards Enter, Escape and navigation keys to that same list.
Held buttons cannot accept a newly opened dialog, and guest input stays neutral
while the overlay captures it. Dialog polling does not process the performance
overlay chord a second time.

Build the `uwp-dialog-input-test` target and run its executable to check the
actual UWP window pump with a simulated controller and dialog consumer. It covers
navigation, A/B confirm/cancel, held-button suppression, keyboard repeat and
keyboard fallback. This does not replace an in-game save/reload check on Xbox.

### Launcher shader preparation

Normal boot now opens a controller/keyboard launcher before starting the game.
Choose **Start game** or **Precompile shaders** using D-pad/arrow keys and A/Enter.
Preparation displays scanning and compilation progress, then returns to the
launcher with the prepared count. Unsupported shaders compile during gameplay.

The current CUSA01715 subset covers 154 resource-free fragment shader identities
(40 distinct programs), including asset-derived input linkage. It checks three
attachment variants per identity and reuses valid saved results on later runs.
Textured fragments and vertex shaders are skipped. Preparation is native C++;
no Python installation or prior playthrough is needed. See
`tools/offline_shaders/README.md` for count definitions and validation.

Results are app-local at `user/shader/precompiled/CUSA01715`. Runtime lookup
requires matching code, specialization and renderer profile; a miss or invalid
file uses the existing compiler. Driver/pipeline compilation can still occur.
Private `graphics-debug.txt` may set `GOW3_PRECOMPILE_AUTORUN=1` for a bounded
launcher smoke test; the default requires selecting the action. Existing
`GOW3_UWP_CAPTURE=1` also writes `user/uwp-launcher.bmp` for launcher/progress QA.
