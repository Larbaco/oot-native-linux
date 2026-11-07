import unittest

import nxs2


class RomfsRequirementsTests(unittest.TestCase):
    def test_glob_is_expanded_and_can_be_empty(self):
        table = "bfsha\tShader/required.bfsha.zs\nglob\tEffect/*.vsetb.byml.zs\n"
        missing, unmatched = nxs2.romfs_requirements(
            {"romfs.marker", "Shader/required.bfsha.zs"}, "romfs.marker", table
        )
        self.assertEqual(missing, [])
        self.assertEqual(unmatched, ["Effect/*.vsetb.byml.zs"])

    def test_glob_matches_romfs_paths(self):
        table = "glob\tEffect/*.vsetb.byml.zs\n"
        missing, unmatched = nxs2.romfs_requirements(
            {"romfs.marker", "Effect/fire.vsetb.byml.zs"}, "romfs.marker", table
        )
        self.assertEqual(missing, [])
        self.assertEqual(unmatched, [])

    def test_marker_and_explicit_shader_paths_remain_required(self):
        table = "sarc\tShader/required.sarc.zs\nglob\tEffect/*.zs\n"
        missing, unmatched = nxs2.romfs_requirements(set(), "romfs.marker", table)
        self.assertEqual(missing, ["romfs.marker", "Shader/required.sarc.zs"])
        self.assertEqual(unmatched, ["Effect/*.zs"])


if __name__ == "__main__":
    unittest.main()
