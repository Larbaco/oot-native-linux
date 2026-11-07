"""The generated game_profile.h of the Ocarina of Time profile (games/0400486024DEC000) carries the values the runtime and the launcher read."""
import pathlib, sys, unittest
ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import gen_game_profile as g

OOT = '0400486024DEC000'


class T(unittest.TestCase):
    def test_oot_is_the_default_and_only_game(self):
        self.assertEqual(g.DEFAULT_GAME, OOT)
        self.assertEqual(sorted(p.name for p in (ROOT / 'games').iterdir() if (p / 'game.tsv').is_file()), [OOT])

    def test_oot_profile_header(self):
        h = g.render(g.DEFAULT_GAME)
        for line in ('#pragma once', f'#define DM_GAME_ID "{OOT}"', '#define DM_GAME_NAME "The Legend of Zelda: Ocarina of Time"',
                     f'#define DM_GAME_PROGRAM_ID 0x{OOT}ull', '#define DM_GAME_MODULES {"main", "subsdk0", "sdk"}', '#define DM_GAME_NMOD 3',
                     '#define DM_GAME_SLUG "oot"', '#define DM_GAME_WINDOW_TITLE "Ocarina of Time"', '#define DM_GAME_DEFAULT_VERSION "2.0.0"',
                     '#define DM_GAME_NVN_API_MAJOR 231', '#define DM_GAME_NVN_API_MINOR 40'):
            self.assertIn(line, h.splitlines())

    def test_render_is_deterministic_and_script_matches(self):
        self.assertEqual(g.render(OOT), g.render(OOT))

    def test_placeholder_fails_cleanly(self):
        with self.assertRaises(SystemExit): g.render('OOT_TBD')


if __name__ == '__main__': unittest.main()
