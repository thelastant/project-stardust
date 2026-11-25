import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools import build_support


class BuildSupportTests(unittest.TestCase):
    def test_validates_eboot_against_configured_pin(self):
        with tempfile.TemporaryDirectory() as temporary:
            eboot = Path(temporary) / "EBOOT.ELF"
            payload = b"test eboot"
            eboot.write_bytes(payload)
            with patch.object(
                build_support,
                "TARGET",
                {"eboot_elf": {"size": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}},
            ):
                build_support.validate_eboot_elf(eboot)

    def test_rejects_eboot_with_wrong_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            eboot = Path(temporary) / "EBOOT.ELF"
            eboot.write_bytes(b"not the pinned executable")
            with patch.object(
                build_support, "TARGET", {"eboot_elf": {"size": eboot.stat().st_size, "sha256": "0" * 64}}
            ):
                with self.assertRaisesRegex(ValueError, "does not match"):
                    build_support.validate_eboot_elf(eboot)

    def test_detects_game_root_from_usrdir(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "disc"
            usrdir = root / "PS3_GAME" / "USRDIR"
            usrdir.mkdir(parents=True)
            eboot = usrdir / "EBOOT.ELF"
            eboot.write_bytes(b"")
            self.assertEqual(build_support.detect_game_dir(eboot), root)

    def test_prepares_npea_output_without_moving_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / "game"
            usrdir = game / "PS3_GAME" / "USRDIR"
            usrdir.mkdir(parents=True)
            (usrdir / "EBOOT.ELF").write_bytes(b"original")
            eboot = root / "EBOOT.ELF"
            eboot.write_bytes(b"selected")
            output = root / "ProjectStardust" / "NPEA00014"

            result = build_support.prepare_game_folder(game, output, eboot)

            self.assertEqual(result, output)
            self.assertEqual((output / "USRDIR" / "EBOOT.ELF").read_bytes(), b"selected")
            self.assertEqual((usrdir / "EBOOT.ELF").read_bytes(), b"original")


if __name__ == "__main__":
    unittest.main()
