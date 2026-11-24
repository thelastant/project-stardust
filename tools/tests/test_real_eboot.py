"""Smoke tests for the user's pinned Super Stardust HD executable."""

import contextlib
import hashlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

from ps3recomp import info

if os.environ.get("SSHD_SKIP_REAL_EBOOT"):
    raise unittest.SkipTest("SSHD_SKIP_REAL_EBOOT is set")

_ELF_PATH = os.environ.get("SSHD_EBOOT_ELF")
_SKIP_REASON = "SSHD_EBOOT_ELF is not set or does not name an existing file"
_TARGET = json.loads((info.REPO_ROOT / "config" / "target.json").read_text(encoding="utf-8"))


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file(), _SKIP_REASON)
class RealEbootPinTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            cls.output = Path(tmp) / "eboot_info.json"
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                cls.exit_code = info.main([_ELF_PATH, "--target-config",
                                           str(info.DEFAULT_CONFIG), "-o", str(cls.output)])
            cls.doc = json.loads(cls.output.read_text(encoding="utf-8"))

    def test_decrypted_executable_matches_target_pin(self):
        self.assertEqual(self.exit_code, 0)
        self.assertEqual(self.doc["input"]["pin"], "verified")
        self.assertEqual(self.doc["input"]["size"], _TARGET["eboot_elf"]["size"])
        self.assertEqual(self.doc["input"]["sha256"], _TARGET["eboot_elf"]["sha256"])

    def test_recovered_function_database_is_pinned_to_same_executable(self):
        database = json.loads((info.REPO_ROOT / "config" / "functions.json").read_text(encoding="utf-8"))
        self.assertEqual(database["input"]["executable_sha256"], _TARGET["eboot_elf"]["sha256"])


if __name__ == "__main__":
    unittest.main()
