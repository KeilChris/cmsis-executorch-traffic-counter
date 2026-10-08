#!/usr/bin/env python3
"""Host-only launch guard tests: no build, debugger or probe invocation."""

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from prepare_launch import IMAGE_BASE, OUTPUT, RUNNER_NAME, prepare_launch


SOURCE = (
    "cbuild-run:\n"
    "  target-type: NuMaker-X-M55M1D\n"
    "  target-set: camera-test\n"
    "  device: Nuvoton::M55M1H2LJAE\n"
    + OUTPUT.replace("      type:", "      info: generate by camera-hil.Release+NuMaker-X-M55M1D\n      type:")
    + "  system-resources:\n"
    "    memory: []\n"
    "  algorithms:\n"
    "    - algorithm: /mock/M55M1_SPIM.FLM\n"
    "      start: 0x82000000\n"
    "      size: 0x02000000\n"
    "      ram-start: 0x20000000\n"
    "      ram-size: 0x00008000\n"
)


class LaunchGuardTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "generated.cbuild-run.yml"
        self.destination = self.root / RUNNER_NAME
        self.source.write_text(SOURCE)
        self.destination.write_text("previous HIL runner\n")
        for suffix in (".axf", ".hex"):
            image = self.root / (IMAGE_BASE + suffix)
            image.parent.mkdir(parents=True, exist_ok=True)
            image.write_bytes(b"fixture, not a real firmware image")

    def assert_refused(self):
        with self.assertRaises((ValueError, FileNotFoundError)):
            prepare_launch(self.source, self.destination)
        self.assertEqual(self.destination.read_text(), "previous HIL runner\n")
        self.assertEqual(len(list(self.root.glob("*.cbuild-run.yml"))), 2)

    def test_valid_runner_keeps_images_source_and_relative_paths(self):
        self.assertEqual(prepare_launch(self.source, self.destination), self.destination.resolve())
        text = self.destination.read_text()
        self.assertIn(IMAGE_BASE + ".axf", text)
        self.assertIn(IMAGE_BASE + ".hex", text)
        self.assertIn("M55M1_HyperRAM.FLM", text)
        self.assertNotIn("M55M1_SPIM.FLM", text)
        self.assertEqual(self.source.read_text(), SOURCE)

    def test_wrong_identity_or_image_layout(self):
        mutations = (
            ("target-type: NuMaker-X-M55M1D", "target-type: AppKit-E7"),
            ("target-set: camera-test", "target-set: default"),
            ("  target-set: camera-test\n", ""),
            ("device: Nuvoton::M55M1H2LJAE", "device: Other"),
            ("camera-hil.axf", "traffic.axf"),
            ("camera-hil.hex", "traffic.hex"),
            ("project: camera-hil", "project: traffic"),
            ("load: symbols", "load: image"),
            ("  output:\n", "  target-set: camera-test\n  output:\n"),
            ("  system-resources:\n", "    - file: extra.hex\n      load: image\n  system-resources:\n"),
        )
        for old, new in mutations:
            with self.subTest(old=old):
                self.source.write_text(SOURCE.replace(old, new))
                self.assert_refused()

    def test_missing_or_empty_images(self):
        for suffix in (".axf", ".hex"):
            with self.subTest(suffix=suffix):
                image = self.root / (IMAGE_BASE + suffix)
                image.unlink()
                self.assert_refused()
                image.touch()
                self.assert_refused()
                image.write_bytes(b"fixture")

    def test_wrong_destination_or_relative_path_base(self):
        for destination in (self.source, self.root / "traffic.cbuild-run.yml", self.root / "nested" / RUNNER_NAME):
            with self.subTest(destination=destination):
                with self.assertRaises(ValueError):
                    prepare_launch(self.source, destination)
        self.assertEqual(self.source.read_text(), SOURCE)

    def test_missing_flash_region_preserves_previous_runner(self):
        self.source.write_text(SOURCE.replace("M55M1_SPIM.FLM", "other.FLM"))
        self.assert_refused()

    def test_prepared_output_is_checked_before_publishing(self):
        def changed_runner(source, destination):
            destination.write_text(SOURCE.replace("camera-hil.hex", "traffic.hex"))
        with patch("prepare_launch.prepare", side_effect=changed_runner):
            self.assert_refused()


if __name__ == "__main__":
    unittest.main(verbosity=2)
