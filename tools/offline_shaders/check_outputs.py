#!/usr/bin/env python3
"""Compare finished asset-only output with cached SPIR-V, after compilation.

The compiler is not run here and no recorded specialization metadata is read.
"""
from __future__ import annotations
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct

NAME = re.compile(r'(0x[0-9a-fA-F]{16})_(\d+)\.spv\Z')


def execution_model(data):
    if len(data) < 20 or len(data) % 4 or struct.unpack_from('<I', data)[0] != 0x07230203:
        raise ValueError('invalid SPIR-V header')
    words = struct.unpack(f'<{len(data) // 4}I', data)
    offset = 5
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 65535
        if not size or offset + size > len(words):
            raise ValueError('invalid SPIR-V instruction length')
        if opcode == 15:
            if size < 4:
                raise ValueError('invalid SPIR-V entry point')
            return words[offset + 1]
        offset += size
    raise ValueError('missing SPIR-V entry point')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('emission', type=Path)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.reference.resolve() in args.output.resolve().parents:
        parser.error('output must be new and outside the reference cache')
    emission = json.loads(args.emission.read_text(encoding='utf-8'))
    if emission['uses_recorded_specializations'] or emission['uses_reference_spirv']:
        parser.error('requires independent asset-only emission')
    if hashlib.sha256((args.reference / 'profile.bin').read_bytes()).hexdigest() != emission['profile_sha256']:
        parser.error('reference and output target profiles differ')
    outputs = defaultdict(list)
    for result in emission['results']:
        if result['outcome'] == 'compiled':
            name = result['shader_hash'] + '-' + result['target'] + '.spv'
            data = (args.emission.parent / name).read_bytes()
            if execution_model(data) != 4:
                raise ValueError('expected fragment output')
            outputs[result['shader_hash']].append((name, data))
    compared, unavailable = [], []
    for file in sorted(args.reference.glob('*.spv')):
        match = NAME.fullmatch(file.name)
        if not match:
            continue
        data = file.read_bytes()
        if execution_model(data) != 4:
            continue
        key = match[1].lower()
        if key not in outputs:
            unavailable.append(file.name)
            continue
        exact = [name for name, output in outputs[key] if output == data]
        compared.append(dict(reference=file.name, exact_outputs=exact,
                             outcome='exact' if exact else 'different'))
    summary = dict(asset_identities_compiled=len(outputs), references_compared=len(compared),
                   exact=sum(x['outcome'] == 'exact' for x in compared),
                   different=sum(x['outcome'] == 'different' for x in compared),
                   asset_identities_without_reference=len(outputs.keys() - {NAME.fullmatch(x['reference'])[1].lower() for x in compared}),
                   reference_fragments_without_asset_output=len(unavailable))
    report = dict(schema_version=1, method='post-compilation comparison only; no .meta input',
                  summary=summary, comparisons=compared, unavailable_references=unavailable)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary))
    return 0 if compared and not summary['different'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
