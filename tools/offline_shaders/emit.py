#!/usr/bin/env python3
"""Compile the currently supported asset-only subset, without a reference cache.

Resource-free fragment headers can use asset-derived input linkage. This
does not constitute a complete per-title runtime shader/pipeline cache.
"""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def select_candidates(shaders, material_plan=None, *, resources=False):
    links = defaultdict(set)
    if material_plan:
        for pair in material_plan['pairs']:
            link = pair['interpolation']
            if all(flag in (0, 8) for flag in link['semantic_flags']) and all(p is not None for p in link['parameter_indices']):
                links[pair['fragment']].add(tuple(link['parameter_indices']))
    candidates, unsupported = [], Counter()
    for shader in shaders:
        if shader['stage'] != 'fragment':
            unsupported['vertex_input_formats'] += 1
        elif shader['usage_slots'] and not resources:
            unsupported['resource_descriptors'] += 1
        elif shader['input_semantics']:
            variants = links[shader['shader_hash']]
            if len(variants) != 1:
                unsupported['missing_or_ambiguous_linkage'] += 1
                continue
            candidates.append((shader, next(iter(variants))))
        else:
            candidates.append((shader, ()))
    return candidates, unsupported


def encode_recipe(shader, parameters, *, resources=False):
    semantics = shader['input_semantics']
    if (shader['usage_slots'] and not resources) or len(semantics) != len(parameters) or any(s & 0xf700 for s in semantics):
        raise ValueError('Unsupported resource or semantic recipe')
    if any(p < 0 or p >= 32 for p in parameters):
        raise ValueError('Invalid interpolation parameter')
    header = struct.pack('<13I', *shader['register_words'])
    header += struct.pack(f'<{len(shader["usage_slots"])}I', *shader['usage_slots'])
    header += struct.pack(f'<{len(semantics)}H', *semantics)
    header += bytes(-len(header) % 4)
    if len(header) != shader['header_bytes']:
        raise ValueError('Asset header size does not match recipe')
    return (struct.pack('<4sIQI', b'GFP1', 2, int(shader['shader_hash'], 16), len(header)) + header +
            struct.pack('<I', len(parameters)) + struct.pack(f'<{len(parameters)}I', *parameters))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory', type=Path)
    parser.add_argument('assets', type=Path)
    parser.add_argument('--profile', type=Path, required=True, help='Target GPU compiler profile.bin')
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--plan', type=Path, help='Asset-derived material linkage plan')
    parser.add_argument('--resources', action='store_true', help='Try explicit float-2D resource variants from asset binding tables')
    parser.add_argument('--hash', action='append', help='Explicit original hash subset; does not provide specialization inputs')
    parser.add_argument('--limit', type=int, help='Explicit bounded experiment size')
    parser.add_argument('--target', choices=('depth-only', 'rgba8', 'bgra8'), action='append', required=True)
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
    material_plan = json.loads(args.plan.read_text(encoding='utf-8')) if args.plan else None
    if material_plan and material_plan['findings']:
        parser.error('requires a material plan without findings')
    candidates, unsupported = select_candidates(assets['recovered'], material_plan, resources=args.resources)
    if args.hash:
        candidates = [(s,p) for s,p in candidates if s['shader_hash'] in args.hash]
    if args.limit is not None:
        if args.limit <= 0:
            parser.error('--limit must be positive')
        candidates = candidates[:args.limit]
    report = {'schema_version': 1, 'uses_recorded_specializations': False,
              'uses_reference_spirv': False, 'whole_game_precompilation': False,
              'profile_sha256': hashlib.sha256(profile.read_bytes()).hexdigest(),
              'asset_shader_count': len(assets['recovered']), 'candidate_fragment_count': len(candidates),
              'unsupported_asset_requirements': dict(unsupported),
              'target_variants': args.target, 'results': []}
    report['descriptor_variant'] = 'float-2d' if args.resources else 'resource-free'
    with tempfile.TemporaryDirectory(dir=output, prefix='inputs-') as temporary:
        recipe = Path(temporary) / 'current.psh'
        for shader, parameters in candidates:
            code = args.inventory.parent / 'objects' / (shader['code_sha256'] + '.gcn')
            if hashlib.sha256(code.read_bytes()).hexdigest() != shader['code_sha256']:
                raise RuntimeError('extracted program digest changed')
            recipe.write_bytes(encode_recipe(shader, parameters, resources=args.resources))
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
                                             code_sha256=shader['code_sha256'], outcome=outcome,
                                             input_parameters=list(parameters)))
                print(name, outcome, flush=True)
    report['summary'] = {outcome: sum(x['outcome'] == outcome for x in report['results'])
                         for outcome in ('compiled', 'failed', 'timeout')}
    (output / 'emission.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report['summary']), flush=True)
    return 0 if report['results'] and not report['summary']['failed'] and not report['summary']['timeout'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
