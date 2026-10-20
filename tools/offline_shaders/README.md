# Offline shader preparation

The UWP launcher offers **Start game** and **Precompile shaders**. Preparation
scans the installed game and invokes the same native C++ shader compiler used by
the renderer. It does not need a prior playthrough or recorded shader metadata.
Python tools in this directory perform extraction and verification only.

For the measured CUSA01715 WAD inventory, the native launcher prepares **all
97,544 original shader identities**: 48,772 fragment and 48,772 vertex shaders.
These share 27,944 distinct instruction sequences. This covers the archived
program inventory, not every possible draw-state specialization. Incompatible
runtime variants retain ordinary on-demand compilation.

## Current results, 15 October 2026

| Measurement | Result |
| --- | ---: |
| WAD files parsed / parsing findings | 410 / 0 |
| Original shader identities prepared | 97,544 / 97,544 |
| Distinct compiled template variants | 98,651 |
| Aliases sharing those compiled records | 338,445 |
| Total preparation checks | 437,096 |
| Skipped identities / preparation failures | 0 / 0 |
| Recorded states satisfied by prepared templates | 516 / 570 |
| Prepared outputs validated and structurally equivalent to references | 516 / 516 |
| Ordinary compiler regression outputs still byte-identical | 270 / 270 |

The reference lookup includes 222 fragment and 294 vertex hits. The remaining
48 fragment and six vertex states safely fall back. Known examples require a
3D image, another attachment configuration, or a different static buffer format.
No encountered-hash allowlist controls asset preparation.

The desktop UWP smoke test reached combat with the ordinary shader cache
disabled and recorded **467 synchronous prepared shader hits**. Sequential
captures showed correctly rendered intros and combat. The test was stopped by
a deadline watchdog and its temporary input/settings were cleared or restored.
Signed Xbox package `1.0.26288.1829` completed native preparation of all 97,544
identities (437,096 variant checks), with zero skips or failures, in approximately
2.5 minutes. Reported memory stayed around 2.0 GiB and returned to 1.7 GiB after
preparation. The console generated its own GPU-profile cache; no PC library was
uploaded. The test closed immediately afterward and manual startup was restored.

The full library prepares on the target GPU profile. A repeat run reuses valid
entries; the desktop launcher's actual preparation action also reused all
97,544 identities. Private evidence is under `xbox/.local/offline-shaders`:
`native-all-templates-02`, `native-all-lookup-02`, `native-all-compare-02`,
`replay-regression-03`, `desktop-all-smoke-02`, and `xbox-all-prepare-02`.

This prepares **PS4 GCN to SPIR-V translation**. Driver translation to DXIL and
graphics pipeline creation can still happen on first use. Original shaders
outside the scanned WAD format and unsupported live variants are not covered
by the inventory total. Complete elimination of first-use stalls is not claimed.

Xbox opening-combat testing recorded 479 synchronous prepared-library hits with
the ordinary cache disabled. Normal caching and a separate run without remote
screenshots also reached combat, but frame-time variability remained. Median
five-second FPS snapshots in the 100–180 second window were approximately 32,
30 and 33 respectively; these are not frame-identical scenes or a before/after
build benchmark. Neither cache disabling nor screenshot traffic alone explains
the reported slowdown. Performance needs a controlled comparison with the prior
build; successful shader preparation does not establish a performance gain.
Private measurements are `xbox-fps-cache-comparison.json` and
`xbox-fps-no-capture.json`. Use `xbox-autostart.ps1 -NoCapture -StopAfter` for
bounded tests without screenshot traffic. All test settings/input are restored
and the app is closed after each run.

## How it scales

The native scanner recovers register headers, resource usage slots, fetch
semantics, and adjacent vertex/fragment linkage from materials. It seeks past
large texture/model payloads and bounds material reads. Compiler pools are
reused between jobs. Shared program/settings combinations compile once; aliases
keep every original identity addressable without duplicating shader bytes.

Fragment preparation enumerates depth-only, RGBA8 and BGRA8 attachment states.
Vertex preparation obtains starting bindings from the paired fragment, covers
both depth ranges, and enumerates float/signed/unsigned bone-index layouts where
needed. The previously unresolved extra vertex semantic flag is accepted as a
linear interpolation flag; unknown flags still fail closed.

Templates defer sampler normalization/gamma, aligned buffer strides, read-only
resource presence, vertex swizzles and read-only formatted-buffer component
selection. Live lookup resolves those constants in already translated SPIR-V.
Other descriptor/layout changes must match a prepared variant. Buffer address
arithmetic preserves the ordinary compiler's integer operation order, including
u32 overflow behavior; writes retain fixed component maps.

Offline resource tables use bounded synthetic memory. Preparation records a
table-read plan rather than executing guest-pointer walker code. Only actual
runtime use registers a deduplicated walker following the current guest tables;
null tables can recover on a later refresh. Prepared records retain no old guest
pointers. The ordinary serialized metadata ABI remains unchanged.

## Storage and runtime checks

Prepared libraries live in `user/shader/precompiled/CUSA01715` within app-local
storage. Each stage has `templates.gtp` plus `templates.gti`; vertex files are
in the `vertex` subdirectory. Library format version 3 is independent of the
ordinary gameplay shader cache.

The measured PC library occupies approximately 2.7 GiB including indexes.
Runtime reads individual shader records; it does not load the entire library
into RAM. Entries check original identity/code, runtime settings, GPU profile,
version, lengths, checksums and fixed specialization fields before reuse.
Missing, incompatible or corrupt entries fall back to the ordinary compiler.

Pack/index writes are staged in `.tmp` files. A completed inventory/profile
match is reused; an interrupted preparation is rebuilt. Cache files depend on
the GPU/compiler profile and should be prepared independently on Xbox. Keep
extracted game bytes, generated caches, dumps and machine configuration in
ignored `.local` directories, outside commits.

## Native tools

With the UWP graphics build configured:

```powershell
pwsh -NoProfile -File tools/offline_shaders/build.ps1
xbox/.local/build/gpu/gow3-offline-shader.exe selftest
xbox/.local/build/gpu/gow3-offline-shader.exe prepare-native <dump> <profile.bin> <output-cache>
```

`prepare-native` exercises the launcher's preparation code without launching a
game or creating a Vulkan device. The profile must come from the target renderer.

For independent extraction and streaming experiments:

```powershell
python tools/offline_shaders/scan.py <dump> --extract --output <new-inventory>
python tools/offline_shaders/assets.py <new-inventory>/report.json --output <new-headers>
python tools/offline_shaders/plan.py <new-inventory>/report.json <new-headers>/assets.json --output <new-plan.json>
python tools/offline_shaders/batch.py <new-headers>/assets.json <new-plan.json> <new-inventory>/objects --stage fragment --output <new-input>
xbox/.local/build/gpu/gow3-offline-shader.exe emit-batch bgra8-template <new-input>/assets.gab <profile.bin> <new-output>
```

For vertices use `--stage vertex` and `vertex-depth-template`.
`vertex-depth-skin-template` selects signed bone indices;
`vertex-depth-uint-template` selects unsigned indices. GAB1 and GFP1/GVP1 recipe
formats are bounded and versioned. The native worker writes a streaming SPIR-V
container and index instead of launching thousands of compiler processes.
`emit-asset TARGET RECIPE PROFILE GCN OUTPUT` handles a single independent case.

## Validation

Asset preparation never consumes gameplay `.meta` files. Only after preparation,
`emit-precompiled CACHE META PROFILE GCN OUTPUT` applies recorded state to test
the real runtime lookup. `freeze-template SPIRV META OUTPUT` tests specialization
separately. `verify-precompiled` checks reuse and incompatible-code/state/profile
rejection against a recorded reference. These are test oracles, not compiler
inputs for the prepared asset library.

Prepared-hit outputs pass the official Vulkan 1.2 validator and are compared
structurally after optimization. Numeric IDs, declaration ordering and unused
globals may differ; executable operations, interfaces, capabilities and execution
modes must agree. At emitted SPIR-V 1.5, `SPV_KHR_float_controls` is redundant but
causes upstream aggressive dead-code elimination to decline the module. The
comparison removes only that declaration and validates the core module before
optimization. All 516 prepared hits agree by this method.

The initial independent batch of 27,207 fragment and 740 vertex jobs separately
passed official Vulkan 1.2 validation. This is distinct from the final 98,651
native variants; it is not a claim that every final variant was independently
validated with SPIRV-Tools. `validate_batch.cpp` is an optional standalone checker
and adds no game dependency.

The ordinary recorded-specialization regression remains byte-identical across
270 cases. Native self-tests cover bounded descriptor reads, fetch mapping,
constant resolution, cache profile/code/state rejection, aliases, presence
constants, malformed records and corruption, and live table-walker null recovery.
Python extraction/recipe checks use synthetic fixtures only:

```powershell
python -m unittest discover -s tools/offline_shaders -p 'test_*.py' -v
```

Desktop game tests must run only one instance, arm a deadline before activation,
and close the app and clear scripted controls on every exit. Console tests must
also close after the requested evidence is collected.

## Inventory provenance and earlier proofs

The initial inventory found 136,006 occurrences, 97,544 original header hashes
and 27,944 distinct instruction sequences. All 48,772 material shader pairs
linked with recovered register settings agreeing with 570 recorded cases.
Seven of 517 raw recorded identities have no matching instruction bytes in this
WAD inventory; generated programs and executable/module layouts are not scanned.

The first independent resource-free group contained 154 identities and matched
all seven available references across depth/RGBA/BGRA variants. That earlier
launcher limit has been replaced by the combined template implementation above.
The textured `bloom` fragment and vertex proofs established byte-identical
asset-derived compilation before expanding to the full library. Python was never
used to generate compiled shaders.

Prior-art research references include [KytyPS5 Shaders Lab](https://github.com/Almo7aya/KytyPS5-Shaders-Lab),
[SCSKiller architecture](https://github.com/BlueHeisenberg/SCSKiller/blob/main/ARCHITECTURE.md),
and [Fossilize](https://github.com/ValveSoftware/Fossilize/blob/master/README.md).
They do not supply this game's complete asset/state reconstruction.
