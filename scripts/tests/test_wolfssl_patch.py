"""Check that a restored wolfSSL settings header keeps its build signature."""

from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import Mock


class WolfSslPatchTest(unittest.TestCase):
    def test_wolfssl_settings_patch_is_idempotent(self):
        script = Path(__file__).resolve().parents[1] / "patch_wolfssl.py"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = Mock()
            env.subst.return_value = directory
            patch = runpy.run_path(str(script), init_globals={"Import": Mock(), "env": env})
            header = root / "user_settings.h"
            header.write_text("#define DEBUG_WOLFSSL\n", encoding="utf-8")

            patch["patch_user_settings"](header)
            first_pass = header.read_bytes()
            self.assertIn(b"#undef DEBUG_WOLFSSL", first_pass)
            self.assertEqual(first_pass.count(patch["MARKER"].encode()), 1)

            patch["patch_user_settings"](header)
            self.assertEqual(header.read_bytes(), first_pass)
