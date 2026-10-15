# Offline shader experiment

This branch tests whether God of War III's original shader programs and their
specializations can be recovered from installed assets before gameplay. The
eventual target is an Xbox launcher action that builds a local cache for the
whole game without requiring a prior playthrough.

The first milestone is a read-only extractor and comparison tool. **It does not
compile shaders or produce an importable runtime cache yet.** No launcher or
runtime behavior changes are included.

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

## Next proof: fresh compilation of one matching case

Use one extracted program plus a known runtime case, compile it freshly, and
compare with its recorded SPIR-V. Then remove the dependence on the runtime
case by reconstructing equivalent inputs from the assets. Repeating a stored
SPIR-V file or replaying only previously collected pipelines would not meet the
whole-game goal.

The compiler needs more than original instruction bytes: shader stages, runtime
register settings, vertex fetch layout, resource formats/descriptors, starting
bindings, and the target GPU profile. Graphics pipeline creation additionally
needs stage combinations and render state. Assets must supply these or permit a
bounded, validated enumeration of variants.

The existing `.meta` files provide useful specialization references but are
native-layout structures and include resource-table walker machine code.
`Info::RefreshFlatBuf` can follow guest pointers. An offline worker must supply
descriptors and fetch data without following old process addresses; loading a
cached specialization is not sufficient to regenerate it safely or independently.
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
python -m unittest discover -s tools/offline_shaders -p test_scan.py -v
```

The synthetic fixtures cover unaligned programs, metadata bounds, duplicate
code/header aliases, truncated archives, count records, reference validation,
and the distinction between identity and byte equality. No game bytes or
machine-specific configuration are included in the tests.
