# Offline shader experiment

This branch tests whether God of War III's original shader programs and their
specializations can be recovered from installed assets before gameplay. The
eventual target is an Xbox launcher action that builds a local cache for the
whole game without requiring a prior playthrough.

The extractor and headless compiler can freshly compile extracted fragment
programs using recorded specialization data. A small resource-free fragment
subset also compiles from asset headers without recorded specialization data.
**A complete asset-derived specialization plan and an importable whole-game
runtime cache are not implemented yet.** No launcher or runtime behavior changes
are included.

## Running the inventory

Python 3.10 or newer; no additional packages, GPU, or running game required.

```powershell
python tools/offline_shaders/scan.py H:/dumps/CUSA01715-app0 `
  --cache <existing-per-title-cache-directory> `
  --dumps <existing-raw-shader-dump-directory> `
  --extract --output xbox/.local/offline-shaders/new-run
```

`--cache`, `--dumps`, and `--extract` are optional. `--archive R_UI.WAD`
selects an archive by basename and may be repeated. `--max-files N` explicitly
limits a scan. The output directory must be new and must not overlap any input.
Keep extracted game bytes and manifests in ignored `.local`, outside commits.

The scanner walks all `EXEC/WAD/PS4_LE/**/*.WAD` files, including `ORIGINAL`.
It understands the 64-byte WAD entry headers, 16-byte payload padding, and the
level archives' payload-free `EntityCount` record. Inside each payload it checks
the shader's `s_mov_b32 vcc_hi` token, metadata pointer, `OrbShdr` signature,
length, and entry bounds. The pointer and length interpretation follows the
renderer in `gpu/shadps4/video_core/amdgpu/regs_shader.h`.

`report.json` records archive parsing results, original shader hashes, code
SHA-256 digests, entry names, offsets, and runtime reference matches. With
`--extract`, `objects/<sha256>.gcn` contains the original instruction bytes,
excluding the usage table and footer. Identical bytes are stored once while all
header identities and origins remain in the manifest. Checkpoints are written
atomically; there is no resume support yet.

The tool distinguishes three different comparisons:

- A cached SPIR-V filename shares the shader's original header hash.
- A runtime raw dump shares that hash **and** contains identical instruction bytes.
- Instruction bytes match but the original header hash differs.

The third case is useful evidence, but cannot substitute for matching runtime
identity. SPIR-V files receive a basic header check, not full validation or a
fresh compiler comparison. Parse findings produce exit code 2, with affected
archives retained in the report. A completed scan is not proof of all pipelines.

## Measured baseline, 15 October 2026

The local CUSA01715 dump and existing desktop UWP references produced:

| Measurement | Result |
| --- | ---: |
| WAD files parsed / parsing findings | 410 / 0 |
| Shader occurrences | 136,006 |
| Distinct original header hashes | 97,544 |
| Distinct instruction byte sequences | 27,944 |
| Cached original shader hashes found in assets | 540 / 549 |
| Raw reference hashes matched by identity and bytes | 508 / 517 |
| Raw reference hashes with identical code under any header | 510 / 517 |

These are inventory and provenance measurements, not a percentage of the whole
game's shaders or pipelines. Seven recorded raw shader identities still have no
matching instruction bytes in this WAD inventory. Their origins need checking;
executables, modules, generated shaders, and alternate program layouts are not
covered. The measured scan took approximately 226 seconds on the development PC.
Private output is under `xbox/.local/offline-shaders/all-wads-02`.

## Fresh compilation comparison

With an existing configured UWP graphics build:

```powershell
pwsh -NoProfile -File tools/offline_shaders/build.ps1
python tools/offline_shaders/compare.py xbox/.local/offline-shaders/all-wads-02/report.json `
  --worker xbox/.local/build/gpu/gow3-offline-shader.exe `
  --output xbox/.local/offline-shaders/new-comparison --limit 270
```

The explicit CMake target is excluded from normal app builds. It links the same
shader compiler as UWP and replaces only its table-refresh pass in this separate
executable: flat descriptor offsets are regenerated, but captured table bytes are
used without following old guest pointers. Recorded walker pointers and machine
code are discarded. Derived compiler Info is built afresh rather than copied.
The tool creates no Vulkan device, launches no game, and never writes into the
reference cache. Each comparison runs in a bounded subprocess and saves its fresh
SPIR-V, log, and provenance in the new private output directory.

Measured result: **270 / 270 extracted fragment-shader cases produced byte-identical
SPIR-V**, including texture/resource-table cases. The initial simple proof was
`0x683cb941b24f0169`, from the `crepuscularRays` material in `R_FX.WAD`. The full
comparison is in `.local/offline-shaders/compile-fragments-03` under `xbox`.
Matching the renderer's `AmdGpu::no_msaa` setting was necessary: without that
initialization, 85 cases emitted multisampled texture operations instead.
The worker uses the UWP default (no MSAA); `GOW3_NO_MSAA=0` selects the other mode.

This is a recorded-specialization replay proof, not whole-game precompilation.
Native `.meta` version 7 and unsalted shader version 7 are required. Vertex fetch,
other stages, and alternate native cache layouts are deliberately rejected.

## Asset-derived register and stage-linkage plan

```powershell
python tools/offline_shaders/assets.py xbox/.local/offline-shaders/all-wads-02/report.json `
  --output xbox/.local/offline-shaders/new-asset-headers
python tools/offline_shaders/plan.py xbox/.local/offline-shaders/all-wads-02/report.json `
  xbox/.local/offline-shaders/new-asset-headers/assets.json `
  --output xbox/.local/offline-shaders/new-material-plan.json
```

`assets.py` reads each inventoried program's packed register header, usage-slot
table, and vertex fetch/export or fragment-input semantic tables. It validates
the relative code location and table sizes. The default output is one manifest;
use `--hash <original-hash> --recipes` to generate a small selection of standalone
recipe files for direct worker experiments. Original game bytes stay private.

`plan.py` pairs adjacent vertex/fragment programs within their actual source
material entries and matches fragment input semantics to vertex exports. Hash
prefix matching alone is insufficient: 68 material pairs have different vertex
and fragment prefixes. Optional `--cases <worker-list.jsonl>` validates recovered
settings against recorded cases; those cases do not build or change the plan.

Measured asset results:

- All **97,544 vertex/fragment register headers** recovered without format failures.
- All **48,772 material shader pairs** linked without missing semantics.
- Recovered common register settings agree with **570 recorded cases**.
- Recovered fragment parameter indices agree with **270 recorded cases**.
- Six materials have an additional semantic flag whose interpretation remains
  unresolved; it is preserved explicitly in the plan.

These counts refer to original identities; many share identical instruction
bytes. The private baseline manifests are `asset-headers-02/assets.json` and
`material-plan-02.json` in `xbox/.local/offline-shaders`.

## Compilation without recorded shader specializations

The worker's `compile-asset TARGET PSH PROFILE GCN REFERENCE OUTPUT` mode builds
fragment settings from the asset header and an explicit target variant. It reads
no `.meta` file. `emit-asset TARGET PSH PROFILE GCN OUTPUT` also requires no
reference SPIR-V. The target GPU's compiler profile is still required; the final
Xbox launcher would obtain that from its renderer/device.

The initial independent depth-only compile of `0x683cb941b24f0169` produced the
same **2,252 SPIR-V bytes** as the recorded reference. The asset's color-export
declaration alone does not identify the draw's active render targets: the
reference used a depth-only pass. An explicit RGBA8 target produces a different
variant, as expected.

```powershell
python tools/offline_shaders/emit.py xbox/.local/offline-shaders/all-wads-02/report.json `
  xbox/.local/offline-shaders/asset-headers-02/assets.json `
  --profile <target-GPU-profile.bin> `
  --worker xbox/.local/build/gpu/gow3-offline-shader.exe `
  --plan xbox/.local/offline-shaders/material-plan-02.json `
  --target depth-only --target rgba8 --target bgra8 `
  --output xbox/.local/offline-shaders/new-independent-batch
```

Measured independent batch: **79 asset shader identities, five distinct code
sequences, two explicit target variants: 158 fresh compilations, zero failures**.
That initial path supported only fragment headers without usage slots/input
semantics. The extended path accepts recovered input linkage from `--plan` and
still rejects shaders that discover resources or user-data specialization. It is
a deliberately small supported subset, not a whole-game cache. Fresh outputs
beyond the byte-matched proof have not been exercised in gameplay.

## Validation method: the cache is a test set

The general asset compiler must obtain its shader-specific inputs from game
assets and explicit target variants. Existing gameplay `.meta` files must not
supply those inputs in this experiment. After compilation has finished, compare
the generated SPIR-V with recorded outputs for the known subset:

```powershell
python tools/offline_shaders/check_outputs.py `
  xbox/.local/offline-shaders/new-independent-batch/emission.json `
  --reference <existing-per-title-cache-directory> `
  --output xbox/.local/offline-shaders/new-independent-comparison.json
```

The comparison tool cannot invoke the compiler and does not read `.meta` files.
Its profile check ensures both sets target the same GPU compiler profile. This
separation lets known examples validate a general method before applying it to
unseen assets. Additional playthroughs are not a prerequisite for expanding it.
The full asset census reports unsupported requirements independently of cache
membership. Representative exact matches support extending the method to other
shaders with the same requirements; they do not guarantee every pipeline variant.

The extended resource-free group has 154 identities (79 without linkage, 75 with
linkage). The first blind run compiled them all, then matched four of seven
available references. The other three differed only in red/blue output channel
ordering, exposing the missing standard BGRA attachment variant. The corrected
batch includes depth-only, RGBA8, and BGRA8 targets. It still uses no recorded
shader specialization inputs.

Corrected result: **462 fresh outputs across 154 identities and three target
variants; all seven available reference cases match byte-for-byte**. The other
147 identities were absent from the reference cache and compiled through the
same asset-only method. This validates extension of the supported group without
requiring a gameplay recording for each member. Private results are
`blind-linkage-02/emission.json` and `blind-linkage-02-validation.json` under
`xbox/.local/offline-shaders`. The full census separately reports 48,618 fragment
identities needing resource descriptors and 48,772 vertex identities needing
input-format support.

## Remaining work

Extend independent compilation to textured materials and vertex shaders by
reconstructing their mesh buffer formats, texture/sampler descriptors, and
binding layout. Resolve the remaining semantic flags and enumerate actual pass
attachment/blend/depth variants before producing runtime-importable entries.
Repeating a stored
SPIR-V file or replaying only previously collected pipelines would not meet the
whole-game goal.

The compiler needs more than original instruction bytes: shader stages, runtime
register settings, vertex fetch layout, resource formats/descriptors, starting
bindings, and the target GPU profile. Graphics pipeline creation additionally
needs stage combinations and render state. Assets must supply these or permit a
bounded, validated enumeration of variants.

The existing `.meta` files provide useful specialization references but are
native-layout structures and include resource-table walker machine code.
`Info::RefreshFlatBuf` can follow guest pointers. The worker supplies captured
descriptors without following old process addresses for replay, but the general
asset-derived descriptor/fetch-data provider still needs implementing.
Keep the experiment outside active runtime caches until fresh output is checked.

Useful prior art: [KytyPS5 Shaders Lab](https://github.com/Almo7aya/KytyPS5-Shaders-Lab)
for offline extraction/compiler experiments,
[SCSKiller architecture](https://github.com/BlueHeisenberg/SCSKiller/blob/main/ARCHITECTURE.md)
for asset-driven pipeline planning, and
[Fossilize](https://github.com/ValveSoftware/Fossilize/blob/master/README.md)
for capturing and replaying explicit Vulkan creation state. None supplies this
game's complete PS4-to-Dozen asset/state reconstruction.

## Checks

```powershell
python -m unittest discover -s tools/offline_shaders -p 'test_*.py' -v
```

The synthetic fixtures cover unaligned programs, metadata bounds, duplicate
code/header aliases, truncated archives, count records, reference validation,
the distinction between identity and byte equality, packed vertex/fragment
tables, semantic mapping, and pairing from source order. No game bytes or
machine-specific configuration are included in the tests.
