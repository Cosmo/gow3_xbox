import struct
import unittest
import check_outputs


class OutputChecks(unittest.TestCase):
    def test_reads_execution_model_from_entry_point(self):
        data = struct.pack('<9I', 0x07230203, 0x10000, 0, 2, 0, (4 << 16) | 15, 4, 1, 0)
        self.assertEqual(check_outputs.execution_model(data), 4)

    def test_rejects_truncated_and_zero_length_instructions(self):
        header = struct.pack('<5I', 0x07230203, 0x10000, 0, 2, 0)
        for data in (header, header + bytes(4), header + struct.pack('<I', (8 << 16) | 15), bytes(24)):
            with self.assertRaises(ValueError):
                check_outputs.execution_model(data)


if __name__ == '__main__':
    unittest.main()
