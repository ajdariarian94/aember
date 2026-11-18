"""The AppImage builder's pure parts and its two scripts."""

import struct
import subprocess
import unittest
import zlib
from pathlib import Path
from unittest import mock

from aember.config import appimage as cfg
from aember.steps.appimage import build

FILES = Path(build.__file__).parent / "files"


class PinnedTest(unittest.TestCase):
    def test_downloads_are_pinned(self):
        for url, digest in (
            (cfg.RUNTIME_URL, cfg.RUNTIME_SHA256),
            (cfg.SLIRP4NETNS_URL, cfg.SLIRP4NETNS_SHA256),
        ):
            self.assertTrue(url.startswith("https://github.com/"), url)
            self.assertRegex(url, r"/download/v\d+\.\d+\.\d+/")  # a release
            self.assertRegex(digest, r"^[0-9a-f]{64}$")


class PayloadTest(unittest.TestCase):
    def test_icon_is_a_valid_png(self):
        png = build.icon_png(32)
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        width, height = struct.unpack(">II", png[16:24])
        self.assertEqual((width, height), (32, 32))
        idat = png.index(b"IDAT")
        length = struct.unpack(">I", png[idat - 4 : idat])[0]
        raw = zlib.decompress(png[idat + 4 : idat + 4 + length])
        self.assertEqual(len(raw), 32 * (1 + 32 * 4))

    def test_desktop_entry(self):
        self.assertIn("Exec=AppRun", build.DESKTOP)
        self.assertIn("Icon=aember", build.DESKTOP)
        self.assertIn("Terminal=false", build.DESKTOP)  # we open our own window

    def test_glibc_is_never_bundled(self):
        ldd = (
            "\tlinux-vdso.so.1 (0x00007ffd)\n"
            "\tlibXaw.so.7 => /usr/lib/x86_64-linux-gnu/libXaw.so.7 (0x1)\n"
            "\tlibc.so.6 => /usr/lib/x86_64-linux-gnu/libc.so.6 (0x2)\n"
            "\tlibm.so.6 => /usr/lib/x86_64-linux-gnu/libm.so.6 (0x3)\n"
            "\t/lib64/ld-linux-x86-64.so.2 (0x4)\n"
            "\tlibfontconfig.so.1 => /usr/lib/libfontconfig.so.1 (0x5)\n"
        )
        result = subprocess.CompletedProcess([], 0, stdout=ldd)
        with mock.patch.object(build.subprocess, "run", return_value=result):
            libs = [p.name for p in build.bundled_libs("/usr/bin/xterm")]
        self.assertEqual(libs, ["libXaw.so.7", "libfontconfig.so.1"])


class ScriptsTest(unittest.TestCase):
    def test_shell_syntax(self):
        for name in ("AppRun", "aember-sandbox"):
            with self.subTest(name=name):
                r = subprocess.run(["sh", "-n", str(FILES / name)], capture_output=True)
                self.assertEqual(r.returncode, 0, r.stderr.decode())

    def test_apprun_help(self):
        r = subprocess.run(
            ["sh", str(FILES / "AppRun"), "--help"], capture_output=True, text=True
        )
        self.assertEqual(r.returncode, 0)
        self.assertIn("pkexec", r.stdout)
        self.assertIn("kill -USR2 1", r.stdout)

    def test_sandbox_isolation(self):
        text = (FILES / "aember-sandbox").read_text()
        start = text.index("unshare --mount")
        unshare = text[start : text.index("&\n", start)]
        for ns in ("--mount", "--pid", "--net", "--cgroup", "--uts", "--ipc"):
            self.assertIn(ns, unshare)
        self.assertIn("--propagation private", unshare)  # mounts never leak
        self.assertIn("--kill-child", unshare)  # helper gone: aember gone
        self.assertIn("--disable-host-loopback", text)  # no way to host's lo
        self.assertIn("exec chroot $R /usr/bin/aember --root --chroot", text)
        self.assertNotIn("--rbind /dev", text)  # own /dev, not the host's
        self.assertIn('mount --bind "$CONSOLE" $R/dev/console', text)
        # The console comes from stage 1: a background job's stdin would be
        # /dev/null (no debug shell, found in the no-FUSE test).
        self.assertIn("CONSOLE=$(tty", text)
        self.assertIn('"$CONSOLE" <&0 &', text)

    def test_apprun_keeps_helper_outside_fuse_and_revokes_x(self):
        text = (FILES / "AppRun").read_text()
        self.assertIn("--appimage-offset", text)
        self.assertIn("xhost -si:localuser:root", text)
        self.assertIn('rm -rf "$W"', text)


if __name__ == "__main__":
    unittest.main()
