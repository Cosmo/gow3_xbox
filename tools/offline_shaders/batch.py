#!/usr/bin/env python3
"""Pack asset recipes and GCN into a streaming input for the native C++ compiler.

This tool does not compile shaders or read gameplay specialization metadata.
Identical code/header/linkage inputs share a job; aliases preserve title hashes.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
from emit import encode_recipe, select_candidates


def vertex_recipe(shader):
    regs = shader['register_words']
    header = struct.pack('<8I', *regs)
    header += struct.pack('<%dI' % len(shader['usage_slots']), *shader['usage_slots'])
    header += struct.pack('<%dI' % len(shader['input_semantics']), *shader['input_semantics'])
    header += struct.pack('<%dH' % len(shader['export_semantics']), *shader['export_semantics'])
    header += bytes(-len(header) % 4)
    if len(header) != shader['header_bytes']:
        raise ValueError('Vertex header size mismatch')
    return struct.pack('<4sIQI', b'GVP1', 1, int(shader['shader_hash'], 16), len(header)) + header


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('assets', type=Path)
    parser.add_argument('plan', type=Path)
    parser.add_argument('objects', type=Path)
    parser.add_argument('--stage', choices=('fragment', 'vertex'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    shaders = json.loads(args.assets.read_text())['recovered']
    if args.stage == 'fragment':
        candidates, skipped = select_candidates(shaders, json.loads(args.plan.read_text()), resources=True)
        skipped.pop('vertex_input_formats', None)
    else:
        candidates = [(s, ()) for s in shaders if s['stage'] == 'vertex']
        skipped = Counter()
    jobs, aliases = {}, []
    for shader, parameters in candidates:
        recipe = encode_recipe(shader, parameters, resources=True) if args.stage == 'fragment' else vertex_recipe(shader)
        # Bytes 8..15 are identity only, not specialization inputs. Code digest
        # and all packed header/linkage bytes remain part of the job signature.
        signature = hashlib.sha256(bytes.fromhex(shader['code_sha256']) + recipe[:8] + recipe[16:]).hexdigest()
        if signature not in jobs:
            jobs[signature] = (shader, recipe)
        aliases.append({'hash': shader['shader_hash'], 'job_hash': jobs[signature][0]['shader_hash'], 'signature': signature})
    args.output.mkdir(parents=True, exist_ok=False)
    with (args.output / 'assets.gab').open('wb') as file:
        file.write(struct.pack('<4sII', b'GAB1', 1, len(jobs)))
        for shader, recipe in jobs.values():
            code = (args.objects / (shader['code_sha256'] + '.gcn')).read_bytes()
            if hashlib.sha256(code).hexdigest() != shader['code_sha256']:
                raise ValueError('Code digest mismatch')
            file.write(struct.pack('<QII', int(shader['shader_hash'], 16), len(recipe), len(code)))
            file.write(recipe)
            file.write(code)
    report = {'stage': args.stage, 'asset_identities': len(aliases), 'native_compilation_jobs': len(jobs),
              'uses_recorded_specializations': False, 'skipped': dict(skipped), 'aliases': aliases}
    (args.output / 'aliases.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({key: value for key, value in report.items() if key != 'aliases'}), flush=True)


if __name__ == '__main__':
    main()
