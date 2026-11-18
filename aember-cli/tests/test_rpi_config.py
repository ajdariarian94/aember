"""Raspberry Pi boot configuration written by `aember rpi image`."""

import re
import unittest

from aember.config import rpi


class RpiConfigTest(unittest.TestCase):
    def test_cmdline_is_one_line_with_root_by_label(self):
        self.assertTrue(rpi.CMDLINE_TXT.endswith("\n"))
        self.assertEqual(rpi.CMDLINE_TXT.count("\n"), 1)
        self.assertIn(f"root=LABEL={rpi.ROOT_LABEL}", rpi.CMDLINE_TXT)
        self.assertIn("rootwait", rpi.CMDLINE_TXT)
        # The last console= is /dev/console: the GPIO serial port.
        consoles = re.findall(r"console=(\S+)", rpi.CMDLINE_TXT)
        self.assertTrue(consoles[-1].startswith("ttyS1"))

    def test_qemu_raspi_boots_without_the_watchdog(self):
        # QEMU's raspi machines reset the moment the BCM2835 watchdog starts.
        self.assertIn("aember.watchdog=0", rpi.QEMU_PI3_APPEND)
        self.assertNotIn("aember.watchdog", rpi.CMDLINE_TXT)

    def test_config_txt_loads_the_pi4_device_tree(self):
        text = rpi.CONFIG_TXT.format(dtb=rpi.DTB_PI4)
        self.assertIn("arm_64bit=1", text)
        self.assertIn("device_tree=bcm2711-rpi-4-b.dtb", text)
        self.assertIn("enable_uart=1", text)

    def test_firmware_is_pinned_by_sha256(self):
        self.assertIn("start4.elf", rpi.FIRMWARE_FILES)
        self.assertIn("fixup4.dat", rpi.FIRMWARE_FILES)
        for name, sha in rpi.FIRMWARE_FILES.items():
            self.assertRegex(sha, r"^[0-9a-f]{64}$", name)

    def test_partition_layout(self):
        self.assertEqual(rpi.BOOT_START % 2048, 0)  # 1 MiB aligned
        self.assertGreaterEqual(rpi.BOOT_SECTORS * 512, 128 * 1024 * 1024)


if __name__ == "__main__":
    unittest.main()
