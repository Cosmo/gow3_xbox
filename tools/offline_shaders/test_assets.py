"""Synthetic packed shader headers; no game data."""
import struct
import unittest
import assets


def header(semantics, slots):
    size = 52 + slots * 4 + ((semantics * 2 + 3) & ~3)
    words = [size + 8, 0xffffffff, 0xc0007, 32, 0, 4, 2, 2, semantics, 0, 16, 15, semantics]
    result = bytearray(struct.pack('<13I', *words) + bytes(slots * 4))
    result.extend(struct.pack(f'<{semantics}H', *range(semantics)))
    result.extend(bytes(size - len(result)))
    return result


class AssetTests(unittest.TestCase):
    def test_vertex_header_has_separate_fetch_and_export_tables(self):
        slots, inputs, exports = 2, 3, 1
        size = 32 + slots * 4 + inputs * 4 + 4
        raw = struct.pack('<8I', size + 8, 0xffffffff, 0xc0007, 32, 0, 4, 0, inputs | exports << 8)
        raw += bytes(slots * 4) + struct.pack('<3I', 0x00040400, 0x00020802, 0x00040c04)
        raw += struct.pack('<H2x', 0x010f)
        found, decoded = assets.vertex_header(b'prefix' + raw, slots)
        self.assertEqual(found, raw)
        self.assertEqual(decoded['export_semantics'], [0x010f])
        self.assertEqual(len(decoded['input_semantics']), 3)

    def test_usage_table_and_odd_semantic_padding(self):
        raw = header(3, 6)
        found, decoded = assets.fragment_header(b'prefix' + raw, 6)
        self.assertEqual(found, raw)
        self.assertEqual(decoded['common'], [16, 0, 32, 0, 0, 0, 3])
        self.assertEqual(decoded['input_semantics'], [0, 1, 2])

    def test_no_semantics_or_usage_slots(self):
        raw = header(0, 0)
        self.assertEqual(assets.fragment_header(raw, 0)[0], raw)

    def test_rejects_bad_relocation_distance_and_truncated_header(self):
        raw = header(3, 6)
        struct.pack_into('<I', raw, 0, 1)
        for candidate in (raw, raw[1:], bytes(64)):
            with self.assertRaises(ValueError):
                assets.fragment_header(candidate, 6)


if __name__ == '__main__':
    unittest.main()
