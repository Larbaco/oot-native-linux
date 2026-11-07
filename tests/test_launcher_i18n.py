import pathlib
import contextlib
import io
import json
import os
import tempfile
import types
import unittest

from oot_native_i18n import (DEFAULT_LANGUAGE, LANGUAGES, STRINGS, resolve_language,
                             translate_builder_error)
from launcher.oot_native_launcher import language_config_path, load_language_config, make_parser, save_language_config
import nxs2


class LauncherLanguageTests(unittest.TestCase):
    def test_every_catalog_key_exists_in_both_languages(self):
        self.assertEqual(set(STRINGS["en"]), set(STRINGS["pt-BR"]))
        self.assertEqual(set(LANGUAGES), set(STRINGS))
        for language, catalog in STRINGS.items():
            with self.subTest(language=language):
                self.assertTrue(all(key and value for key, value in catalog.items()))

    def test_default_language_is_english_and_ignores_system_locale(self):
        self.assertEqual("en", DEFAULT_LANGUAGE)
        self.assertEqual("en", resolve_language(environ={"LANG": "pt_BR.UTF-8", "LC_ALL": "pt_BR.UTF-8"}))
        self.assertEqual("en", resolve_language(environ={}, config={}))

    def test_portuguese_builder_error_preserves_technical_cause(self):
        message = "no shader was translated; first exception: FileNotFoundError: shader fixture missing"
        translated = translate_builder_error(message, "pt-BR")
        self.assertTrue(translated.startswith("A build falhou."))
        self.assertIn(message, translated)

    def test_explicit_language_precedence_and_config_round_trip(self):
        self.assertEqual("pt-BR", resolve_language(environ={"OOT_NATIVE_LANG": "pt-BR"}))
        self.assertEqual("en", resolve_language("en", {"OOT_NATIVE_LANG": "pt-BR"}))
        self.assertEqual("pt-BR", make_parser("pt-BR").parse_args(["--lang", "pt-BR", "--check", "fixture.nup"]).lang)
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "oot-native" / "launcher.json"
            self.assertTrue(save_language_config("pt-BR", path))
            self.assertEqual({"language": "pt-BR"}, load_language_config(path))
            self.assertEqual("pt-BR", resolve_language(config=load_language_config(path)))
            self.assertEqual(pathlib.Path(tmp) / "oot-native" / "launcher.json",
                             language_config_path({"XDG_CONFIG_HOME": tmp}))

    def test_broken_language_config_is_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "launcher.json"
            path.write_bytes(b"not json\xff")
            self.assertEqual({}, load_language_config(path))

    def test_check_dump_json_code_is_language_independent(self):
        with tempfile.TemporaryDirectory() as tmp:
            dump = pathlib.Path(tmp) / "synthetic.nup"
            dump.write_bytes(b"synthetic invalid fixture")
            codes = []
            previous = os.environ.get("OOT_NATIVE_LANG")
            try:
                for language in LANGUAGES:
                    os.environ["OOT_NATIVE_LANG"] = language
                    out = io.StringIO()
                    with contextlib.redirect_stdout(out):
                        rc = nxs2.check_dump(types.SimpleNamespace(dump=str(dump), json=True))
                    self.assertEqual(1, rc)
                    codes.append(json.loads(out.getvalue())["code"])
            finally:
                if previous is None:
                    os.environ.pop("OOT_NATIVE_LANG", None)
                else:
                    os.environ["OOT_NATIVE_LANG"] = previous
            self.assertEqual(["invalid_format", "invalid_format"], codes)
            self.assertEqual("wrong_game_version", nxs2.classify_dump_error("this recipe is for another game"))


if __name__ == "__main__":
    unittest.main()
