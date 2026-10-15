#!/usr/bin/env python3
"""Compile the currently supported asset-only subset, without a reference cache.

Only resource-free fragment headers with no input linkage are supported. This
does not constitute a complete per-title runtime shader/pipeline cache.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory', type=Path)
    parser.add_argument('assets', type=Path)
    parser.add_argument('--profile', type=Path, required=True, help='Target GPU compiler profile.bin')
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--target', choices=('depth-only', 'rgba8'), action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text(encoding='utf-8'))
    assets = json.loads(args.assets.read_text(encoding='utf-8'))
    if inventory['status'] != 'completed' or assets['failures']:
        parser.error('requires completed inventories without findings')
    output, worker, profile = args.output.resolve(), args.worker.resolve(), args.profile.resolve()
    for source in (args.inventory.parent.resolve(), args.assets.parent.resolve(), profile.parent):
        if output == source or source in output.parents or output in source.parents:
            parser.error('output must not overlap inputs')
    output.mkdir(parents=True, exist_ok=False)
    candidates = [s for s in assets['recovered'] if s['stage'] == 'fragment' and s['header_bytes'] == 52
                  and not s['input_semantics'] and not s['usage_slots']]
    report = {'schema_version': 1, 'uses_recorded_specializations': False,
              'uses_reference_spirv': False, 'whole_game_precompilation': False,
              'profile_sha256': hashlib.sha256(profile.read_bytes()).hexdigest(),
              'asset_shader_count': len(assets['recovered']), 'candidate_fragment_count': len(candidates),
              'target_variants': args.target, 'results': []}
    with tempfile.TemporaryDirectory(dir=output, prefix='inputs-') as temporary:
        recipe = Path(temporary) / 'current.psh'
        for shader in candidates:
            code = args.inventory.parent / 'objects' / (shader['code_sha256'] + '.gcn')
            if hashlib.sha256(code.read_bytes()).hexdigest() != shader['code_sha256']:
                raise RuntimeError('extracted program digest changed')
            header = struct.pack('<13I', *shader['register_words'])
            recipe.write_bytes(struct.pack('<4sIQI', b'GFP1', 1, int(shader['shader_hash'], 16), len(header)) + header)
            for target in dict.fromkeys(args.target):
                name = shader['shader_hash'] + '-' + target
                command = [str(worker), 'emit-asset', target, str(recipe), str(profile),
                           str(code.resolve()), str(output / (name + '.spv'))]
                try:
                    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                    outcome = 'compiled' if result.returncode == 0 else 'failed'
                    log = result.stdout + result.stderr
                except subprocess.TimeoutExpired:
                    outcome, log = 'timeout', 'Worker exceeded 30 seconds; terminated.\n'
                (output / (name + '.log')).write_text(log, encoding='utf-8')
                report['results'].append(dict(shader_hash=shader['shader_hash'], target=target,
                                             code_sha256=shader['code_sha256'], outcome=outcome))
                print(name, outcome, flush=True)
    report['summary'] = {outcome: sum(x['outcome'] == outcome for x in report['results'])
                         for outcome in ('compiled', 'failed', 'timeout')}
    (output / 'emission.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report['summary']), flush=True)
    return 0 if report['results'] and not report['summary']['failed'] and not report['summary']['timeout'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
