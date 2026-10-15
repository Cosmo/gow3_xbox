"""Synthetic archive/reference tests; no proprietary game bytes or GPU required."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

import scan


def shader(shader_hash=0x12345678abcdef01):
    code = scan.TOKEN + struct.pack('<I', 7) + bytes(24)
    info = scan.SIGNATURE + bytes([7]) + struct.pack('<I4xQI', (len(code) << 8) | 7, shader_hash, 0)
    return code + bytes(64 - len(code)) + info, code


def entry(name, payload):
    encoded = name.encode('ascii')
    return struct.pack('<HHI56s', 8, 1, len(payload), encoded) + payload + bytes(-len(payload) % 16)


class ScanTests(unittest.TestCase):
    def test_unaligned_program_pointer_and_excluded_metadata(self):
        payload, code = shader()
        data = b'!' + payload
        result = scan.shader_at(data, 1, len(data))
        self.assertEqual(result['header_offset'], 65)
        self.assertEqual(result['code_bytes'], len(code))
        self.assertEqual(result['code_sha256'], hashlib.sha256(code).hexdigest())

    def test_rejects_missing_footer_and_out_of_entry_pointer(self):
        payload, _ = shader()
        self.assertIsNone(scan.shader_at(payload, 0, 70))
        self.assertIsNone(scan.shader_at(scan.TOKEN + b'\xff' * 4 + payload, 0, len(payload)))
        self.assertIsNone(scan.shader_at(bytes(96), 0, 96))

    def test_rejects_code_extending_past_footer(self):
        payload, _ = shader()
        bad = bytearray(payload)
        struct.pack_into('<I', bad, 72, (68 << 8) | 7)
        self.assertIsNone(scan.shader_at(bad, 0, len(bad)))

    def test_archive_deduplicates_code_preserves_header_identities(self):
        payload, code = shader()
        other, _ = shader(0xfedcba9876543210)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / 'test.WAD'
            path.write_bytes(entry('first', b'!' + payload) + entry('second', payload) + entry('other', other))
            objects = {}
            report = scan.scan_archive(path, path.name, objects, root)
            self.assertFalse(report['findings'])
            self.assertEqual(report['shader_occurrences'], 3)
            self.assertEqual(len(objects), 2)
            self.assertEqual(len(list(root.glob('*.gcn'))), 1)
            first = objects[('0x12345678abcdef01', hashlib.sha256(code).hexdigest())]
            self.assertEqual(len(first['origins']), 2)
            self.assertEqual(first['origins'][0]['code_offset'], 65)

    def test_invalid_archive_is_reported(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'broken.WAD'
            path.write_bytes(struct.pack('<HHI56s', 8, 1, 100000, b'invalid'))
            report = scan.scan_archive(path, path.name, {}, None)
            self.assertIn('exceeds archive bounds', report['findings'][0])

    def test_level_entity_count_is_not_a_payload_size(self):
        payload, _ = shader()
        count = struct.pack('<HHI56s', 0, 0, 161, b'EntityCount')
        entries = list(scan.wad_entries(count + entry('shader entity', payload)))
        self.assertEqual(len(entries), 2)
        self.assertEqual(entries[0]['payload_start'], entries[0]['payload_end'])
        self.assertEqual(entries[0]['entry_value'], 161)
        self.assertEqual(entries[1]['entry_offset'], 64)

    def test_hash_match_is_not_claimed_as_byte_exact(self):
        payload, code = shader()
        item = scan.shader_at(payload, 0, len(payload))
        compiled = {item['shader_hash']: ['reference.spv']}
        dumps = {item['shader_hash']: [{'file': 'different.bin', 'sha256': '0' * 64}]}
        result = scan.summary({'one': item}, compiled, dumps, [], 1, 2)
        self.assertEqual(result['cached_hashes_found_in_archives'], 1)
        self.assertEqual(result['raw_dump_hashes_with_byte_exact_match'], 0)
        self.assertFalse(result['all_archive_files_scanned'])
        self.assertFalse(result['whole_game_pipeline_coverage_proven'])

    def test_reference_names_and_spirv_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / '0x12345678abcdef01_0.spv').write_bytes(struct.pack('<5I', 0x07230203, 0x10500, 0, 1, 0))
            (root / 'fs_0x12345678abcdef01_0.bin').write_bytes(b'example')
            (root / 'vs_0x12345678abcdef01.fetch.bin').write_bytes(b'not a full shader')
            compiled, bytecode = scan.references(root, root)
            self.assertEqual(len(compiled), 1)
            self.assertEqual(len(bytecode['0x12345678abcdef01']), 1)
            (root / '0x12345678abcdef01_0.spv').write_bytes(bytes(20))
            with self.assertRaises(ValueError):
                scan.references(root, root)

    def test_identical_code_with_different_header_hash_is_reported_separately(self):
        payload, code = shader()
        item = scan.shader_at(payload, 0, len(payload))
        dumps = {'0x0000000000000001': [{'file': 'alias.bin',
                                        'sha256': hashlib.sha256(code).hexdigest()}]}
        result = scan.summary({'one': item}, {}, dumps, [], 1, 1)
        self.assertEqual(result['raw_dump_hashes_with_byte_exact_match'], 0)
        self.assertEqual(result['raw_dump_hashes_with_identical_code_any_header'], 1)
        self.assertEqual(result['raw_dump_files_with_identical_code_any_header'], 1)
        self.assertFalse(result['reference_raw_hashes_without_identical_code'])


if __name__ == '__main__':
    unittest.main()
