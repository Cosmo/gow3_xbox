#!/usr/bin/env python3
"""Recover vertex/fragment headers from WAD assets, without runtime metadata."""
from __future__ import annotations
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import struct


def fragment_header(prefix: bytes, usage_slots: int):
    """Validate the packed PS header, usage slots, and padded semantic table.

    The first register word encodes the distance to code plus eight; its paired
    address word is an on-disk relocation sentinel. Do not treat it as an address.
    """
    candidates = []
    for semantics in range(33):
        size = 52 + usage_slots * 4 + ((semantics * 2 + 3) & ~3)
        if size > len(prefix):
            continue
        header = prefix[-size:]
        words = struct.unpack_from('<13I', header)
        if words[0] != size + 8 or words[1] != 0xffffffff or words[12] != semantics:
            continue
        rsrc1, rsrc2 = words[2:4]
        common = [(rsrc2 >> 1) & 31, (rsrc1 >> 24) & 3, ((rsrc1 & 63) + 1) * 4,
                  (rsrc1 >> 12) & 3, (rsrc1 >> 14) & 3,
                  (rsrc1 >> 16) & 3, (rsrc1 >> 18) & 3]
        if common[0] > 16:
            continue
        candidates.append((header, {
            'header_bytes': size, 'register_words': list(words), 'common': common,
            'usage_slots': list(struct.unpack_from(f'<{usage_slots}I', header, 52)),
            'input_semantics': list(struct.unpack_from(f'<{semantics}H', header, 52 + usage_slots * 4)),
        }))
    if len(candidates) != 1:
        raise ValueError(f'expected one validated fragment header, found {len(candidates)}')
    return candidates[0]


def vertex_header(prefix: bytes, usage_slots: int):
    candidates = []
    for size in range(32, min(len(prefix), 32 + 255 * 4 + 32 * 4 + 64) + 1, 4):
        header = prefix[-size:]
        words = struct.unpack_from('<8I', header)
        inputs, exports = words[7] & 255, (words[7] >> 8) & 255
        if words[0] != size + 8 or words[1] != 0xffffffff or words[7] >> 16 or inputs > 32 or exports > 32:
            continue
        if size != 32 + usage_slots * 4 + inputs * 4 + ((exports * 2 + 3) & ~3):
            continue
        rsrc1, rsrc2 = words[2:4]
        common = [(rsrc2 >> 1) & 31, (rsrc1 >> 24) & 3, ((rsrc1 & 63) + 1) * 4,
                  (rsrc1 >> 12) & 3, (rsrc1 >> 14) & 3, (rsrc1 >> 16) & 3, (rsrc1 >> 18) & 3]
        if common[0] > 16:
            continue
        candidates.append((header, {
            'header_bytes': size, 'register_words': list(words), 'common': common,
            'usage_slots': list(struct.unpack_from(f'<{usage_slots}I', header, 32)),
            'input_semantics': list(struct.unpack_from(f'<{inputs}I', header, 32 + usage_slots * 4)),
            'export_semantics': list(struct.unpack_from(f'<{exports}H', header, 32 + usage_slots * 4 + inputs * 4)),
        }))
    if len(candidates) != 1:
        raise ValueError(f'expected one validated vertex header, found {len(candidates)}')
    return candidates[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--hash', action='append', help='Explicit original hash subset')
    parser.add_argument('--recipes', action='store_true', help='Emit standalone recipe files (prefer a small --hash subset)')
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text(encoding='utf-8'))
    if inventory['status'] != 'completed':
        parser.error('requires a completed inventory without archive findings')
    game, output = Path(inventory['game_root']).resolve(), args.output.resolve()
    for source in (game, args.inventory.parent.resolve()):
        if output == source or source in output.parents or output in source.parents:
            parser.error('output must not overlap inputs')
    output.mkdir(parents=True, exist_ok=False)
    if args.recipes:
        (output / 'recipes').mkdir()
    results, failures = [], []
    with ExitStack() as stack:
        files = {}
        for shader in inventory['shaders']:
            if shader['header_type'] not in (0, 1) or args.hash and shader['shader_hash'] not in args.hash:
                continue
            origin = shader['origins'][0]
            archive = origin['archive']
            if archive not in files:
                files[archive] = stack.enter_context((game / archive).open('rb'))
            file = files[archive]
            prefix_size = min(1536, origin['code_offset'] - origin['entry_offset'] - 64)
            file.seek(origin['code_offset'] - prefix_size)
            prefix = file.read(prefix_size)
            file.seek(origin['header_offset'])
            footer = file.read(28)
            try:
                if len(footer) != 28 or footer[:7] != b'OrbShdr' or struct.unpack_from('<Q', footer, 16)[0] != int(shader['shader_hash'], 16):
                    raise ValueError('shader footer changed since inventory')
                fragment = shader['header_type'] == 0
                header, decoded = (fragment_header if fragment else vertex_header)(prefix, footer[13])
                if args.recipes:
                    recipe = output / 'recipes' / (shader['shader_hash'] + ('.psh' if fragment else '.vsh'))
                    recipe.write_bytes(struct.pack('<4sIQI', b'GFP1' if fragment else b'GVP1', 1, int(shader['shader_hash'], 16), len(header)) + header)
                results.append(dict(shader_hash=shader['shader_hash'], code_sha256=shader['code_sha256'],
                                    stage='fragment' if fragment else 'vertex', origin=origin, **decoded))
            except ValueError as error:
                failures.append(dict(shader_hash=shader['shader_hash'], error=str(error), origin=origin))
    report = {'schema_version': 2, 'uses_runtime_metadata': False,
              'scope': 'vertex/fragment register and semantic headers, not complete specializations/pipelines',
              'recovered': results, 'failures': failures}
    (output / 'assets.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(recovered=len(results), failed=len(failures))), flush=True)
    return 0 if results and not failures else 2


if __name__ == '__main__':
    raise SystemExit(main())
