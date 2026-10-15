import unittest
import plan


class PlanTests(unittest.TestCase):
    def test_pair_by_source_order_not_hash_prefix(self):
        def shader(stage, key, offset):
            return dict(header_type=stage, shader_hash=key, origins=[
                dict(archive='level.WAD', entry_offset=64, code_offset=offset)])
        pairs, findings = plan.material_pairs([shader(0, 'different-prefix', 300), shader(1, 'vertex', 200)])
        self.assertFalse(findings)
        self.assertEqual(list(pairs), [('vertex', 'different-prefix')])

    def test_unpaired_stage_is_a_finding(self):
        pairs, findings = plan.material_pairs([dict(header_type=0, shader_hash='fragment', origins=[
            dict(archive='level.WAD', entry_offset=64, code_offset=200)])])
        self.assertFalse(pairs)
        self.assertTrue(findings)

    def test_semantics_link_in_fragment_order_and_preserve_unknown_flags(self):
        result = plan.interpolation(dict(export_semantics=[0x020f, 0x0110]),
                                    dict(input_semantics=[0x0010, 0x080f, 0x0011]))
        self.assertEqual(result['parameter_indices'], [1, 2, None])
        self.assertEqual(result['semantic_flags'], [0, 8, 0])

    def test_duplicate_vertex_semantics_are_not_silently_overwritten(self):
        with self.assertRaises(ValueError):
            plan.interpolation(dict(export_semantics=[0x020f, 0x010f]), dict(input_semantics=[15]))


if __name__ == '__main__':
    unittest.main()
