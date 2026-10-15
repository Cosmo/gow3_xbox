#!/usr/bin/env python3
"""Read-only GoW III WAD shader inventory and comparison with runtime references.

This establishes original-bytecode provenance, not offline compilation or complete
runtime pipeline coverage. Generated manifests and shader bytes belong in .local.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import mmap
from pathlib import Path
import re
import struct
import time
import zlib

TOKEN = b'\xff\x03\xeb\xbe'
SIGNATURE = b'OrbShdr'
SPV_NAME = re.compile(r'0x([0-9a-fA-F]{16})_(\d+)\.spv\Z')
DUMP_NAME = re.compile(r'([a-z]+)_0x([0-9a-fA-F]{16})_(\d+)\.bin\Z')


def shader_at(data, start: int, end: int):
    """Validate the same header pointer used by AmdGpu::SearchBinaryInfo.

    WAD payloads need not be aligned in the file. The code's second word locates
    BinaryInfo; its length excludes the usage table and BinaryInfo itself.
    """
    if start < 0 or start + 8 > end or data[start:start + 4] != TOKEN:
        return None
    delta = (struct.unpack_from('<I', data, start + 4)[0] + 1) * 8
    if delta > 65536 or delta < 8:
        return None
    info = start + delta
    if info + 28 > end or data[info:info + 7] != SIGNATURE:
        return None
    packed = struct.unpack_from('<I', data, info + 8)[0]
    length = packed >> 8
    if not length or length % 4 or length > delta:
        return None
    code = data[start:start + length]
    return {
        'shader_hash': f'0x{struct.unpack_from("<Q", data, info + 16)[0]:016x}',
        'code_sha256': hashlib.sha256(code).hexdigest(),
        'code_bytes': length,
        'header_version': data[info + 7],
        'header_type': (packed >> 2) & 15,
        'header_offset': info,
        'header_crc32': f'0x{struct.unpack_from("<I", data, info + 24)[0]:08x}',
        'code_crc32': f'0x{zlib.crc32(code):08x}',
    }


def wad_entries(data):
    """Walk the flat 64-byte entry headers already used by scripts/glyphs.py."""
    offset = 0
    while offset + 64 <= len(data):
        tag, flags, size = struct.unpack_from('<HHI', data, offset)
        name = bytes(data[offset + 8:offset + 64]).split(b'\0')[0].decode('latin1')
        # Level WADs have a count record followed immediately by entity headers.
        # Unlike the resource WADs used by glyphs.py, this field is not a byte size.
        payload_size = 0 if tag == 0 and name == 'EntityCount' else size
        end = offset + 64 + payload_size
        if end > len(data):
            raise ValueError(f'entry at {offset:#x} exceeds archive bounds')
        yield {'entry_offset': offset, 'entry_name': name, 'entry_tag': tag,
               'entry_flags': flags, 'entry_value': size,
               'payload_start': offset + 64, 'payload_end': end}
        offset += 64 + ((payload_size + 15) & ~15)
    if any(data[offset:]):
        raise ValueError(f'nonzero trailing bytes at {offset:#x}')


def references(cache: Path | None, dumps: Path | None):
    compiled, bytecode = {}, {}
    if cache:
        for path in sorted(cache.glob('*.spv')):
            match = SPV_NAME.fullmatch(path.name)
            if not match:
                continue
            with path.open('rb') as source:
                header = source.read(20)
            if len(header) != 20 or struct.unpack_from('<I', header)[0] != 0x07230203:
                raise ValueError(f'invalid reference SPIR-V: {path.name}')
            key = f'0x{int(match[1], 16):016x}'
            compiled.setdefault(key, []).append(path.name)
    if dumps:
        for path in sorted(dumps.glob('*.bin')):
            match = DUMP_NAME.fullmatch(path.name)
            if not match:
                continue
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            key = f'0x{int(match[2], 16):016x}'
            bytecode.setdefault(key, []).append({'file': path.name, 'sha256': digest})
    return compiled, bytecode


def scan_archive(path: Path, relative: str, objects: dict, output: Path | None):
    report = {'archive': relative, 'bytes': path.stat().st_size, 'entries': 0,
              'shader_occurrences': 0, 'findings': []}
    if not report['bytes']:
        report['findings'].append('empty archive')
        return report
    with path.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        try:
            for entry in wad_entries(data):
                report['entries'] += 1
                position = entry['payload_start']
                while True:
                    position = data.find(TOKEN, position, entry['payload_end'])
                    if position < 0:
                        break
                    shader = shader_at(data, position, entry['payload_end'])
                    if shader:
                        report['shader_occurrences'] += 1
                        origin = {k: v for k, v in entry.items() if k.startswith('entry_')}
                        origin.update(archive=relative, code_offset=position,
                                      header_offset=shader.pop('header_offset'))
                        key = (shader['shader_hash'], shader['code_sha256'])
                        if key not in objects:
                            objects[key] = dict(shader, origins=[])
                            if output:
                                target = output / (shader['code_sha256'] + '.gcn')
                                if not target.exists():
                                    target.write_bytes(data[position:position + shader['code_bytes']])
                        objects[key]['origins'].append(origin)
                    position += 4
        except ValueError as error:
            report['findings'].append(str(error))
    return report


def summary(objects, compiled, bytecode, reports, selected, total):
    extracted = {shader['shader_hash'] for shader in objects.values()}
    code_hashes = {shader['code_sha256'] for shader in objects.values()}
    exact = {shader['shader_hash'] for shader in objects.values()
             if any(dump['sha256'] == shader['code_sha256']
                    for dump in bytecode.get(shader['shader_hash'], []))}
    code_matched = {key for key, dumps in bytecode.items()
                    if any(dump['sha256'] in code_hashes for dump in dumps)}
    return {
        'archives_selected': selected, 'archives_available': total,
        'archives_completed': len(reports),
        'all_archive_files_scanned': selected == total == len(reports),
        'archives_with_findings': sum(bool(r['findings']) for r in reports),
        'unique_bytecode_objects': len({shader['code_sha256'] for shader in objects.values()}),
        'shader_identities': len(objects), 'unique_shader_header_hashes': len(extracted),
        'shader_occurrences': sum(r['shader_occurrences'] for r in reports),
        'reference_cached_shader_hashes': len(compiled),
        'reference_raw_shader_hashes': len(bytecode),
        'cached_hashes_found_in_archives': len(extracted & compiled.keys()),
        'raw_dump_hashes_found_in_archives': len(extracted & bytecode.keys()),
        'raw_dump_hashes_with_byte_exact_match': len(exact),
        'raw_dump_hashes_with_identical_code_any_header': len(code_matched),
        'raw_dump_files_with_identical_code_any_header': sum(
            dump['sha256'] in code_hashes for dumps in bytecode.values() for dump in dumps),
        'reference_cached_hashes_not_found': sorted(compiled.keys() - extracted),
        'reference_raw_hashes_not_found': sorted(bytecode.keys() - extracted),
        'reference_raw_hashes_without_identical_code': sorted(bytecode.keys() - code_matched),
        'whole_game_pipeline_coverage_proven': False,
    }


def write_report(path: Path, report):
    temporary = path.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--cache', type=Path, help='Existing per-title plain-file pipeline cache')
    parser.add_argument('--dumps', type=Path, help='Existing raw shader dumps, optional comparison oracle')
    parser.add_argument('--output', type=Path, required=True, help='New output directory outside game/cache/dumps')
    parser.add_argument('--archive', action='append', help='Select archive basename; repeated selections allowed')
    parser.add_argument('--max-files', type=int, help='Explicit partial-scan bound')
    parser.add_argument('--extract', action='store_true', help='Retain deduplicated original code in output/objects')
    args = parser.parse_args()
    game, output = args.game.resolve(), args.output.resolve()
    for source in (game, args.cache, args.dumps):
        if source:
            source = source.resolve()
            if not source.is_dir():
                parser.error(f'input is not a directory: {source}')
            if output == source or source in output.parents or output in source.parents:
                parser.error('output and input directories must not overlap')
    if not (game / 'eboot.bin').is_file():
        parser.error('game root must contain eboot.bin')
    if args.max_files is not None and args.max_files <= 0:
        parser.error('--max-files must be positive')
    paths = sorted(p for p in (game / 'EXEC' / 'WAD' / 'PS4_LE').rglob('*')
                   if p.is_file() and p.suffix.lower() == '.wad')
    selected = [p for p in paths if not args.archive or p.name in args.archive]
    if args.archive and set(args.archive) - {p.name for p in selected}:
        parser.error('one or more selected archive names were not found')
    if args.max_files:
        selected = selected[:args.max_files]
    if not selected:
        parser.error('no WAD archives selected')
    compiled, bytecode = references(args.cache, args.dumps)
    output.mkdir(parents=True, exist_ok=False)
    object_dir = output / 'objects' if args.extract else None
    if object_dir:
        object_dir.mkdir()
    started = time.monotonic()
    objects, reports = {}, []
    last_saved = 0.0
    report = {'schema_version': 1, 'purpose': 'offline original-shader provenance experiment',
              'limitations': ['No shader compilation or runtime pipeline cache generation.',
                             'Header-pointer shaders in plain WAD payloads only; no decompression.',
                             'Hashes and even exact bytecode do not establish specialization coverage.'],
              'game_root': str(game), 'reference_cache': str(args.cache) if args.cache else None,
              'reference_dumps': str(args.dumps) if args.dumps else None,
              'status': 'scanning', 'summary': {}, 'archives': reports, 'shaders': []}
    for path in selected:
        reports.append(scan_archive(path, path.relative_to(game).as_posix(), objects, object_dir))
        report['summary'] = summary(objects, compiled, bytecode, reports, len(selected), len(paths))
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        if time.monotonic() - last_saved >= 5 or len(reports) == len(selected):
            report['shaders'] = [dict(shader, cached_spirv=compiled.get(shader['shader_hash'], []),
                                      byte_exact_dumps=[d['file'] for d in bytecode.get(shader['shader_hash'], [])
                                                        if d['sha256'] == shader['code_sha256']])
                                 for shader in objects.values()]
            write_report(output / 'report.json', report)
            last_saved = time.monotonic()
        print(f'{len(reports)}/{len(selected)} {path.name}: '
              f'{reports[-1]["shader_occurrences"]} occurrences; '
              f'{len(objects)} shader identities; '
              f'{report["summary"]["raw_dump_hashes_with_byte_exact_match"]} exact reference hashes', flush=True)
    report['status'] = 'completed_with_findings' if any(r['findings'] for r in reports) else 'completed'
    write_report(output / 'report.json', report)
    print(json.dumps({k: v for k, v in report['summary'].items() if not isinstance(v, list)}, indent=2), flush=True)
    return 0 if report['status'] == 'completed' else 2


if __name__ == '__main__':
    raise SystemExit(main())
