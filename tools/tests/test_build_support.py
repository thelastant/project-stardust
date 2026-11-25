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
            self.assertEqual(build_support.detect_game_dir(eboot), root.resolve())

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

            self.assertEqual(result, output.resolve())
            self.assertEqual((output / "USRDIR" / "EBOOT.ELF").read_bytes(), b"selected")
            self.assertEqual((usrdir / "EBOOT.ELF").read_bytes(), b"original")

    def test_prepares_artwork_from_npea_folder_for_executable_resources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / "NPEA00014"
            game.mkdir()
            png = b"\x89PNG\r\n\x1a\n" + bytes(8) + (64).to_bytes(4, "big") + (64).to_bytes(4, "big") + b"image"
            (game / "ICON0.PNG").write_bytes(png)
            (game / "PIC0.PNG").write_bytes(png)
            output = root / "delivery" / "NPEA00014"
            output.mkdir(parents=True)
            with patch.object(build_support, "ROOT", root):
                icon, pic = build_support.prepare_game_artwork(game, output)

            resource = root / "generated" / "setup-game-artwork"
            self.assertEqual(icon.read_bytes(), png)
            self.assertEqual(pic.read_bytes(), png)
            rc = (resource / "game_artwork.rc").read_text(encoding="utf-8")
            self.assertIn("IDI_ICON1 ICON", rc)
            self.assertIn("PIC0_ARTWORK RCDATA", rc)
            self.assertTrue((resource / "ICON0.ico").is_file())


if __name__ == "__main__":
    unittest.main()
