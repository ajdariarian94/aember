"""QEMU command lines built by `aember app launch`."""

import unittest

from aember.steps.launching.launch import QEMU_PROFILES, extra_kernel_args


class QemuProfileTest(unittest.TestCase):
    def test_every_profile_has_net_and_the_pci_watchdog(self):
        for arch, profile in QEMU_PROFILES.items():
            with self.subTest(arch=arch):
                cmd = profile.base_cmd("k", "i", "r.img", "aember.shell=1")
                self.assertEqual(cmd[0], profile.binary)
                self.assertIn("i6300esb", cmd)
                self.assertEqual(cmd[cmd.index("i6300esb") - 1], "-device")
                self.assertIn("virtio-net-pci,netdev=net0", cmd)
                append = cmd[cmd.index("-append") + 1]
                self.assertIn(profile.root, append)
                self.assertTrue(append.endswith("aember.shell=1"))
                self.assertIn("file=r.img," + profile.drive, cmd)

    def test_headless_has_no_display(self):
        from dataclasses import replace

        for arch, profile in QEMU_PROFILES.items():
            with self.subTest(arch=arch):
                cmd = replace(profile, display=()).base_cmd("k", "i", "r")
                self.assertEqual(cmd[-4:], ["-display", "none", "-vga", "none"])


class ExtraKernelArgsTest(unittest.TestCase):
    def test_debug_shell_and_append(self):
        self.assertEqual(extra_kernel_args(False, ""), "")
        self.assertEqual(extra_kernel_args(True, ""), "aember.shell=1")
        self.assertEqual(
            extra_kernel_args(True, "  aember.watchdog=0   quiet "),
            "aember.shell=1 aember.watchdog=0 quiet",
        )
        self.assertEqual(
            extra_kernel_args(False, "aember.watchdog=30"), "aember.watchdog=30"
        )


if __name__ == "__main__":
    unittest.main()
