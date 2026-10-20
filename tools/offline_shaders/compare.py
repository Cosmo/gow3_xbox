#!/usr/bin/env python3
"""Fresh compilation of extracted programs with recorded specialization oracles."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory', type=Path, help='Completed scan report.json with extracted objects')
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New private output directory')
    parser.add_argument('--limit', type=int, default=1)
    args = parser.parse_args()
    if args.limit <= 0:
        parser.error('--limit must be positive')
    inventory = json.loads(args.inventory.read_text(encoding='utf-8'))
    if inventory['status'] != 'completed':
        parser.error('requires an inventory completed without archive findings')
    cache = Path(inventory['reference_cache']).resolve()
    output, worker = args.output.resolve(), args.worker.resolve()
    for source in (cache, args.inventory.parent.resolve()):
        if output == source or source in output.parents or output in source.parents:
            parser.error('output must not overlap cache or extraction inputs')
    output.mkdir(parents=True, exist_ok=False)
    listed = subprocess.run([str(worker), 'list', str(cache)], capture_output=True,
                            text=True, timeout=60)
    (output / 'list.log').write_text(listed.stderr, encoding='utf-8')
    (output / 'cases.jsonl').write_text(listed.stdout, encoding='utf-8')
    if listed.returncode:
        raise RuntimeError('worker could not inspect native metadata; see list.log')
    extracted = {s['shader_hash']: s for s in inventory['shaders']}
    candidates = []
    for line in listed.stdout.splitlines():
        case = json.loads(line)
        shader = extracted.get(case['hash'])
        if case['stage'] != 0 or case['fetch'] or not shader:
            continue
        reference = cache / f'{case["hash"]}_{case["permutation"]}.spv'
        code = args.inventory.parent / 'objects' / (shader['code_sha256'] + '.gcn')
        if reference.is_file() and code.is_file():
            candidates.append((case, shader, reference, code))
    candidates.sort(key=lambda c: (c[0]['buffers'] + c[0]['images'] + c[0]['samplers'],
                                   c[1]['code_bytes'], c[0]['hash'], c[0]['permutation']))
    report = {'schema_version': 1, 'uses_recorded_specializations': True,
              'whole_game_precompilation': False, 'candidate_cases': len(candidates),
              'results': []}
    for case, shader, reference, code in candidates[:args.limit]:
        if hashlib.sha256(code.read_bytes()).hexdigest() != shader['code_sha256']:
            raise RuntimeError('extracted bytecode digest changed')
        name = f'{case["hash"]}_{case["permutation"]}'
        started = time.monotonic()
        command = [str(worker), 'compile', str(cache / case['meta']), str(cache / 'profile.bin'),
                   str(code.resolve()), str(reference), str(output / (name + '.spv'))]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            log = result.stdout + result.stderr
            outcome = 'exact' if result.returncode == 0 else 'different' if result.returncode == 3 else 'failed'
            record = dict(case, outcome=outcome, returncode=result.returncode,
                          elapsed_seconds=round(time.monotonic() - started, 3))
        except subprocess.TimeoutExpired:
            log = 'Compiler worker exceeded 30 seconds; terminated.\n'
            record = dict(case, outcome='timeout')
        (output / (name + '.log')).write_text(log, encoding='utf-8')
        record['code_sha256'] = shader['code_sha256']
        record['asset_origin'] = shader['origins'][0]
        report['results'].append(record)
        (output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(name, record['outcome'], flush=True)
    counts = {outcome: sum(r['outcome'] == outcome for r in report['results'])
              for outcome in ('exact', 'different', 'failed', 'timeout')}
    print(json.dumps(dict(candidates=len(candidates), attempted=len(report['results']), **counts)), flush=True)
    return 0 if report['results'] and counts['exact'] == len(report['results']) else 2


if __name__ == '__main__':
    raise SystemExit(main())
