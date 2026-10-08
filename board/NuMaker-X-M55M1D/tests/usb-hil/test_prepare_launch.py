"""Offline launch identity/address guards; never access hardware."""
from pathlib import Path
import json
import tempfile
import unittest
import prepare_launch as p


def record(kind, data, offset=0):
    value = bytes([len(data), offset >> 8, offset & 255, kind]) + bytes(data)
    return ":" + (value + bytes([-sum(value) & 255])).hex().upper() + "\n"


class LaunchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="usb-hil-launch-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        image = self.root / p.IMAGE_BASE
        image.parent.mkdir(parents=True)
        image.with_suffix(".axf").write_bytes(b"test fixture, not a real ELF")
        self.hex = record(4, [0, 16]) + record(0, [1, 2, 3, 4]) + record(1, [])
        image.with_suffix(".hex").write_text(self.hex)
        self.text = ("cbuild-run:\n  target-type: NuMaker-X-M55M1D\n  target-set: usb-test\n"
                     f"  device: Nuvoton::M55M1H2LJAE\n  device-pack: {p.DEVICE_PACK}\n" + p.OUTPUT)

    def test_valid_internal_image(self):
        self.assertEqual(p.validate(self.text, self.root), 4)

    def test_wrong_target_set(self):
        for target in ("camera-test", "", "default"):
            with self.assertRaisesRegex(ValueError, "target-set"):
                p.validate(self.text.replace("usb-test", target), self.root)

    def test_wrong_program(self):
        with self.assertRaisesRegex(ValueError, "only USB HIL"):
            p.validate(self.text.replace("usb-hil.axf", "traffic.axf"), self.root)

    def test_wrong_pack(self):
        for version in ("3.1.5", "3.1.4-rc.5"):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "device-pack"):
                p.validate(self.text.replace(p.DEVICE_PACK, "Nuvoton::NuMicroM55_DFP@" + version), self.root)

    def test_external_or_ram_image(self):
        for base in ([0x82, 0], [0x20, 0], [0, 0]):
            with self.assertRaisesRegex(ValueError, "outside internal flash"):
                p.validate_hex(record(4, base) + record(0, [1]) + record(1, []))

    def test_checksum_eof_and_empty(self):
        for broken in (self.hex[:-2], self.hex[:-12], record(1, []), self.hex + record(0, [1])):
            with self.assertRaises(ValueError):
                p.validate_hex(broken)

    def test_missing_output(self):
        (self.root / (p.IMAGE_BASE + ".axf")).unlink()
        with self.assertRaisesRegex(ValueError, "missing/empty"):
            p.validate(self.text, self.root)

    def test_preserve_old_runner_on_failure(self):
        source = self.root / "generated.yml"
        destination = self.root / p.RUNNER_NAME
        source.write_text(self.text.replace("usb-test", "camera-test"))
        destination.write_text("old runner")
        with self.assertRaises(ValueError):
            p.prepare_launch(source, destination)
        self.assertEqual(destination.read_text(), "old runner")
        source.write_text(self.text)
        self.assertEqual(p.prepare_launch(source, destination), 4)
        self.assertEqual(destination.read_text(), self.text)


class TaskConfigurationTests(unittest.TestCase):
    def test_load_uses_shell_and_preserves_validation(self):
        root = Path(__file__).resolve().parents[4]
        for relative in (".vscode.d/tasks.json", ".vscode/tasks.json"):
            with self.subTest(file=relative):
                tasks = json.loads((root / relative).read_text())["tasks"]
                matches = [t for t in tasks if t["label"] == "NuMaker USB HIL Load"]
                self.assertEqual(len(matches), 1)
                task = matches[0]
                self.assertEqual(task["type"], "shell")
                self.assertEqual(task["command"], "pyocd")
                self.assertEqual(task["osx"]["options"]["shell"],
                                 {"executable": "/bin/bash", "args": ["-c"]})
                self.assertEqual(task["dependsOn"], "NuMaker USB HIL Prepare")
                self.assertEqual(task["dependsOrder"], "sequence")
                self.assertEqual(task["args"], [
                    "load", "--probe", "cmsisdap:", "--cbuild-run",
                    "${workspaceFolder}/out/cmsis-executorch+NuMaker-X-M55M1D.usb-hil.cbuild-run.yml",
                ])


if __name__ == "__main__":
    unittest.main()
