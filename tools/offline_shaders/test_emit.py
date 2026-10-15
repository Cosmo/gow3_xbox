import struct
import unittest
import emit


def fragment(key='0x0000000000000001', inputs=(15,)):
    size = 52 + ((len(inputs) * 2 + 3) & ~3)
    return dict(shader_hash=key, stage='fragment', usage_slots=[], input_semantics=list(inputs),
                header_bytes=size, register_words=[size + 8, 0xffffffff, 0xc0000, 4, 0, 4,
                                                  2, 2, len(inputs), 0, 16, 15, len(inputs)])


class EmitTests(unittest.TestCase):
    def test_linked_recipe_contains_the_asset_map_and_padded_header(self):
        shader = fragment(inputs=(15, 16, 17))
        recipe = emit.encode_recipe(shader, (2, 0, 1))
        magic, version, key, size = struct.unpack_from('<4sIQI', recipe)
        self.assertEqual((magic, version, key, size), (b'GFP1', 2, 1, 60))
        self.assertEqual(struct.unpack_from('<4I', recipe, 20 + size), (3, 2, 0, 1))

    def test_unknown_flags_and_out_of_range_parameters_are_rejected(self):
        for shader, parameters in ((fragment(inputs=(0x040f,)), (0,)), (fragment(), (32,))):
            with self.assertRaises(ValueError):
                emit.encode_recipe(shader, parameters)

    def test_linear_interpolation_flag_preserves_asset_parameter_linkage(self):
        shader = fragment(inputs=(0x080f,))
        material = dict(pairs=[dict(fragment=shader['shader_hash'], interpolation=dict(
            parameter_indices=[3], semantic_flags=[8]))])
        selected, unsupported = emit.select_candidates([shader], material, resources=True)
        self.assertEqual([(s['shader_hash'], p) for s, p in selected], [(shader['shader_hash'], (3,))])
        self.assertFalse(unsupported)
        self.assertEqual(struct.unpack_from('<H', emit.encode_recipe(shader, (3,)), 20 + 52), (0x080f,))

    def test_resource_recipe_keeps_asset_usage_slots_before_semantics(self):
        shader = fragment()
        shader['usage_slots'] = [0x0002011b, 0x03040200, 0x000c0201, 0x00100002]
        shader['header_bytes'] += 16
        shader['register_words'][0] += 16
        with self.assertRaises(ValueError):
            emit.encode_recipe(shader, (0,))
        data = emit.encode_recipe(shader, (0,), resources=True)
        self.assertEqual(struct.unpack_from('<4I', data, 20+52), tuple(shader['usage_slots']))
        self.assertEqual(struct.unpack_from('<H', data, 20+52+16)[0], 15)

    def test_asset_requirements_not_historical_cache_membership_select_candidates(self):
        simple = fragment('0x0000000000000002', ())
        linked = fragment()
        resource = dict(fragment('0x0000000000000003'), usage_slots=[1])
        vertex = dict(fragment('0x0000000000000004'), stage='vertex')
        material = dict(pairs=[dict(fragment=linked['shader_hash'], interpolation=dict(
            parameter_indices=[3], semantic_flags=[0]))])
        selected, unsupported = emit.select_candidates([simple, linked, resource, vertex], material)
        self.assertEqual([(s['shader_hash'], p) for s, p in selected], [(simple['shader_hash'], ()), (linked['shader_hash'], (3,))])
        self.assertEqual(dict(unsupported), dict(resource_descriptors=1, vertex_input_formats=1))


if __name__ == '__main__':
    unittest.main()
