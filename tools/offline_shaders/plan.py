#!/usr/bin/env python3
"""Link asset vertex/fragment pairs and compare recovered state with known cases.

This is a partial pipeline plan: texture/mesh formats, attachment variants, and
non-default interpolation flags still need reconstruction.
"""
from __future__ import annotations
import argparse
from collections import defaultdict
import json
from pathlib import Path


def material_pairs(shaders):
    entries = defaultdict(list)
    for shader in shaders:
        for origin in shader['origins']:
            entries[(origin['archive'], origin['entry_offset'])].append(
                (origin['code_offset'], shader['header_type'], shader['shader_hash'], origin))
    pairs, findings = {}, []
    for location, programs in entries.items():
        programs.sort(key=lambda x: x[0])
        if len(programs) % 2 or any(v[1] != 1 or p[1] != 0 for v, p in zip(programs[::2], programs[1::2])):
            findings.append(dict(archive=location[0], entry_offset=location[1], error='expected alternating vertex/fragment programs'))
            continue
        for vertex, fragment in zip(programs[::2], programs[1::2]):
            # Source adjacency is evidence; a shared hash prefix alone is not.
            pairs.setdefault((vertex[2], fragment[2]), vertex[3])
    return pairs, findings


def interpolation(vertex, fragment):
    exported = {}
    for semantic in vertex['export_semantics']:
        name, parameter = semantic & 255, semantic >> 8
        if name in exported:
            raise ValueError('duplicate vertex export semantic')
        exported[name] = parameter
    return {
        'parameter_indices': [exported.get(s & 255) for s in fragment['input_semantics']],
        'semantic_flags': [s >> 8 for s in fragment['input_semantics']],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory', type=Path)
    parser.add_argument('assets', type=Path)
    parser.add_argument('--cases', type=Path, help='Optional worker list JSONL; validation only')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output must be new')
    inventory = json.loads(args.inventory.read_text(encoding='utf-8'))
    assets = json.loads(args.assets.read_text(encoding='utf-8'))
    if inventory['status'] != 'completed' or assets['failures']:
        parser.error('requires completed inventories without findings')
    recovered = {s['shader_hash']: s for s in assets['recovered']}
    pairs, findings = material_pairs(inventory['shaders'])
    linked, missing, unusual = [], 0, 0
    by_fragment = defaultdict(list)
    for (vertex, fragment), origin in pairs.items():
        if vertex not in recovered or fragment not in recovered:
            findings.append(dict(vertex=vertex, fragment=fragment, error='missing asset header'))
            continue
        try:
            linkage = interpolation(recovered[vertex], recovered[fragment])
        except ValueError as error:
            findings.append(dict(vertex=vertex, fragment=fragment, error=str(error)))
            continue
        missing += any(x is None for x in linkage['parameter_indices'])
        unusual += any(linkage['semantic_flags'])
        record = dict(vertex=vertex, fragment=fragment, origin=origin, interpolation=linkage,
                      resource_formats_recovered=False, render_target_variants_enumerated=False)
        linked.append(record)
        by_fragment[fragment].append(record)
    validation = {'common_matched': 0, 'interpolation_parameters_matched': 0, 'not_in_assets': 0, 'differences': []}
    if args.cases:
        for line in args.cases.read_text(encoding='utf-8').splitlines():
            case = json.loads(line)
            shader = recovered.get(case['hash'])
            if not shader:
                validation['not_in_assets'] += 1
                continue
            if shader['common'] == case['common']:
                validation['common_matched'] += 1
            else:
                validation['differences'].append(dict(hash=case['hash'], field='common'))
            if case['stage'] == 0:
                candidates = by_fragment[case['hash']]
                if any(p['interpolation']['parameter_indices'] == [x & 255 for x in case['input_map']]
                       for p in candidates):
                    validation['interpolation_parameters_matched'] += 1
                else:
                    validation['differences'].append(dict(hash=case['hash'], field='interpolation_parameters'))
    report = {'schema_version': 1, 'whole_game_pipeline_coverage_proven': False,
              'summary': dict(material_shader_pairs=len(pairs), linked_pairs=len(linked),
                              pairs_with_missing_semantics=missing, pairs_with_unresolved_flags=unusual,
                              findings=len(findings)),
              'validation': validation, 'findings': findings, 'pairs': linked}
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(report['summary'], validation=validation)))
    return 0 if not findings and not validation['differences'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
