"""x86-64 USB stick image, its QEMU test boot, flashing checks and the
release's disk images (Raspberry Pi Imager repository)."""

import datetime as dt
import json
import lzma
import re
import tempfile
import unittest
from pathlib import Path

from aember.config import rpi, usb
from aember.steps.releasing import images
from aember.steps.rpi import flash
from aember.steps.usb.image import write_esp_tree
from aember.steps.usb.launch import DOWN, ENTER, MenuPicker, qemu_command


class UsbConfigTest(unittest.TestCase):
    def test_default_entry_puts_the_console_on_the_screen(self):
        name, _title, options = usb.ENTRIES[0]
        self.assertIn(f"default {name}", usb.LOADER_CONF)
        consoles = re.findall(r"console=(\S+)", options)
        self.assertEqual(consoles[-1], "tty0")  # last = /dev/console

    def test_serial_entry_puts_the_console_on_ttys0(self):
        _name, title, options = usb.ENTRIES[1]
        self.assertIn("serial", title)
        self.assertTrue(re.findall(r"console=(\S+)", options)[-1].startswith("ttyS0"))

    def test_every_entry_finds_root_by_label(self):
        for _name, _title, options in usb.ENTRIES:
            self.assertIn(f"root=LABEL={usb.ROOT_LABEL}", options)
        self.assertIn("aember.shell=1", usb.ENTRIES[2][2])

    def test_partition_layout(self):
        self.assertEqual(usb.ESP_START % 2048, 0)  # 1 MiB aligned
        self.assertGreaterEqual(usb.ESP_SECTORS * 512, 128 * 1024 * 1024)
        self.assertRegex(usb.SYSTEMD_BOOT_SHA256, r"^[0-9a-f]{64}$")
        # The release pocket: never removed from the archive.
        self.assertNotIn("-updates", usb.SYSTEMD_BOOT_URL)


class EspTreeTest(unittest.TestCase):
    def test_boot_loader_kernel_initramfs_and_entries(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            pkg = t / "pkg"
            pkg.mkdir()
            (pkg / "kernel").write_bytes(b"MZkernel")
            (pkg / "aember-initramfs.cpio.gz").write_bytes(b"initrd")
            efi = t / "systemd-bootx64.efi"
            efi.write_bytes(b"MZboot")
            esp = t / "esp"
            write_esp_tree(esp, pkg, efi, "1.2.3")

            self.assertEqual((esp / "EFI/BOOT/BOOTX64.EFI").read_bytes(), b"MZboot")
            self.assertEqual((esp / usb.KERNEL).read_bytes(), b"MZkernel")
            self.assertEqual((esp / usb.INITRAMFS).read_bytes(), b"initrd")
            entries = sorted((esp / "loader/entries").iterdir())
            self.assertEqual(
                [e.name for e in entries], sorted(n for n, _, _ in usb.ENTRIES)
            )
            text = (esp / "loader/entries/aember.conf").read_text()
            self.assertIn("title aember\n", text)
            self.assertIn("version 1.2.3\n", text)
            self.assertIn(f"linux /{usb.KERNEL}\n", text)
            self.assertIn(f"initrd /{usb.INITRAMFS}\n", text)
            self.assertIn("sort-key aember-1\n", text)

            # Rebuilding starts from scratch.
            (esp / "stale").write_text("x")
            write_esp_tree(esp, pkg, efi, "1.2.3")
            self.assertFalse((esp / "stale").exists())


class UsbLaunchTest(unittest.TestCase):
    def test_boots_the_image_as_a_usb_stick_with_uefi(self):
        cmd = qemu_command(Path("/x/a.img"), Path("/fw/OVMF.fd"), headless=True)
        joined = " ".join(cmd)
        self.assertIn("-bios /fw/OVMF.fd", joined)
        self.assertIn("if=none,id=stick,file=/x/a.img,format=raw", joined)
        self.assertIn("usb-storage,bus=xhci.0,drive=stick,bootindex=0", joined)
        self.assertIn("-snapshot", cmd)  # the image stays what gets flashed
        self.assertIn("i6300esb", cmd)
        self.assertIn("user,model=e1000e", cmd)  # a real PC NIC driver
        self.assertEqual(cmd[cmd.index("-display") + 1], "none")
        window = qemu_command(Path("/x/a.img"), Path("/fw/OVMF.fd"), headless=False)
        self.assertEqual(window[window.index("-display") + 1], "gtk")

    def test_menu_picker_selects_the_serial_entry_once(self):
        picker = MenuPicker("aember (serial console)")
        self.assertEqual(picker.feed(b"\x1b[2J  aember   aember (ser"), b"")
        self.assertEqual(picker.feed(b"ial console)  Boot in 3 s."), DOWN + ENTER)
        self.assertEqual(picker.feed(b"aember (serial console)"), b"")
        self.assertTrue(picker.done)


class FlashChecksTest(unittest.TestCase):
    def dev(self, size, removable=True, model="Flash Disk"):
        return flash.BlockDevice("/dev/sdx", "sdx", size, removable, model)

    def test_usb_sticks_may_be_bigger_than_sd_cards(self):
        size = 300 * 1024**3
        self.assertTrue(flash.problems(self.dev(size), 1, "", False))  # SD default
        self.assertEqual(
            flash.problems(
                self.dev(size), 1, "", False, usb.MAX_STICK_BYTES, "a USB stick"
            ),
            [],
        )

    def test_refuses_virtual_mounted_small_and_fixed_disks(self):
        errs = flash.problems(
            self.dev(10, removable=False, model="Virtual Disk"),
            100,
            "/dev/sdx1 /media vfat rw 0 0\n",
            False,
            usb.MAX_STICK_BYTES,
            "a USB stick",
        )
        text = " ".join(errs)
        for word in ("virtual disk", "not removable", "smaller", "mounted"):
            self.assertIn(word, text)
        self.assertIn("not a USB stick", text)

    def test_pi_limit_unchanged(self):
        self.assertEqual(rpi.MAX_CARD_BYTES, 256 * 1024**3)


class ReleaseImagesTest(unittest.TestCase):
    def test_compress_round_trip_and_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            img = t / "aember-rpi4-v1.2.3.img"
            img.write_bytes(b"\0" * 100000 + b"aember")
            info = images.compress(img, t / "out")
            xz = t / "out" / "aember-rpi4-v1.2.3.img.xz"
            self.assertEqual(lzma.decompress(xz.read_bytes()), img.read_bytes())
            self.assertEqual(info["extract_size"], img.stat().st_size)
            self.assertEqual(info["image_download_size"], xz.stat().st_size)
            sha_line = (t / "out" / (xz.name + ".sha256")).read_text()
            self.assertEqual(sha_line.split(), [info["image_download_sha256"], xz.name])

    def test_imager_repository(self):
        info = {
            "extract_size": 10,
            "extract_sha256": "a" * 64,
            "image_download_size": 5,
            "image_download_sha256": "b" * 64,
        }
        doc = images.imager_repo("me/aember", "1.2.3", info, dt.date(2026, 11, 19))
        (entry,) = doc["os_list"]
        self.assertEqual(
            entry["url"],
            "https://github.com/me/aember/releases/download/v1.2.3/"
            "aember-rpi4-v1.2.3.img.xz",
        )
        self.assertTrue(entry["icon"].endswith("/v1.2.3/aember-icon.png"))
        self.assertEqual(entry["devices"], ["pi4-64bit"])
        self.assertEqual(entry["release_date"], "2026-11-19")
        for key, value in info.items():
            self.assertEqual(entry[key], value)
        json.dumps(doc)

    def test_release_images_only_this_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = images.images_dir(root)
            out.mkdir(parents=True)
            for name in (
                "aember-rpi4-v1.2.3.img.xz",
                "aember-rpi4-v1.2.3.img.xz.sha256",
                "aember-x64-usb-v1.2.3.img.xz",
                "aember-x64-usb-v1.2.3.img.xz.sha256",
                "aember-x64-usb-v1.0.0.img.xz",
                images.IMAGER_ICON,
            ):
                (out / name).write_text("x")
            (out / images.IMAGER_JSON).write_text('{"url": ".../v1.2.3/..."}')
            names = [p.name for p in images.release_images(root, "1.2.3")]
            self.assertNotIn("aember-x64-usb-v1.0.0.img.xz", names)
            self.assertIn("aember-x64-usb-v1.2.3.img.xz.sha256", names)
            self.assertIn(images.IMAGER_JSON, names)
            self.assertIn(images.IMAGER_ICON, names)
            # An Imager list of another version is not uploaded.
            (out / images.IMAGER_JSON).write_text('{"url": ".../v1.0.0/..."}')
            names = [p.name for p in images.release_images(root, "1.2.3")]
            self.assertNotIn(images.IMAGER_JSON, names)


if __name__ == "__main__":
    unittest.main()
