"""The .aember format's Python side (aember/utils/aember_package.py) and
its Ed25519."""

import base64
import json
import lzma
import tempfile
import unittest
from pathlib import Path

from aember.utils import aember_package as pkg
from aember.utils import ed25519

# RFC 8032, 7.1, tests 1 and 2.
RFC = [
    (
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
        "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    ),
    (
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da08"
        "5ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    ),
]


class Ed25519Test(unittest.TestCase):
    def test_rfc8032_vectors(self):
        for seed, public, message, signature in RFC:
            seed_b, msg = bytes.fromhex(seed), bytes.fromhex(message)
            self.assertEqual(ed25519.public_key(seed_b).hex(), public)
            self.assertEqual(ed25519.sign(seed_b, msg).hex(), signature)
            self.assertTrue(
                ed25519.verify(bytes.fromhex(public), msg, bytes.fromhex(signature))
            )

    def test_verify_rejects(self):
        seed, public, _, signature = RFC[0]
        pub, sig = bytes.fromhex(public), bytes.fromhex(signature)
        self.assertFalse(ed25519.verify(pub, b"x", sig))
        bad = bytearray(sig)
        bad[5] ^= 1
        self.assertFalse(ed25519.verify(pub, b"", bytes(bad)))
        self.assertFalse(ed25519.verify(pub[:31], b"", sig))
        self.assertFalse(ed25519.verify(pub, b"", sig[:63]))
        with self.assertRaises(ValueError):
            ed25519.public_key(b"short")


SEED = bytes.fromhex(RFC[0][0])
PUB = bytes.fromhex(RFC[0][1])
CONFIG = {"name": "app", "command": ["/app"], "restart_policy": "always"}


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.image = self.dir / "image.squashfs"
        self.image.write_bytes(b"hsqs" + bytes(5000))

    def tearDown(self):
        self.tmp.cleanup()

    def make(self, seed=SEED, config=CONFIG, name="app.aember"):
        out = self.dir / name
        pkg.write_package(out, config, "lxc.uts.name = app\n", self.image, seed)
        return out

    def test_round_trip_signed(self):
        out = self.make()
        footer = pkg.read_footer(out)
        self.assertTrue(footer.signed)
        self.assertEqual(footer.key_id, pkg.key_id(PUB))
        self.assertEqual(pkg.verify(out, {"dev": PUB}), "dev")
        config = pkg.extract(out, self.dir / "x")
        self.assertEqual(config, CONFIG)
        self.assertEqual(
            (self.dir / "x" / "image.squashfs").read_bytes(), self.image.read_bytes()
        )
        self.assertEqual(
            (self.dir / "x" / "lxc.container.conf").read_text(), "lxc.uts.name = app\n"
        )

    def test_layout(self):
        """The bytes as doc/aember-format.md describes them."""
        raw = self.make().read_bytes()
        footer = raw[-160:]
        self.assertEqual(footer[:8], b"AEMBERv1")
        self.assertEqual(footer[152:], b"1vREBMEA")
        size = int.from_bytes(footer[8:16], "little")
        self.assertEqual(size, len(raw) - 160)
        payload = raw[:size]
        self.assertEqual(payload[:6], b"\xfd7zXZ\x00")  # one xz stream
        tar = lzma.decompress(payload)
        self.assertEqual(tar[257:265], b"ustar\x0000")
        self.assertEqual(tar[:11], b"config.json")
        self.assertTrue(
            ed25519.verify(PUB, b"aember-package-v1\0" + footer[:88], footer[88:152])
        )

    def test_reproducible(self):
        a = self.make().read_bytes()
        b = self.make().read_bytes()
        self.assertEqual(a, b)

    def test_unsigned(self):
        out = self.make(seed=None)
        self.assertFalse(pkg.read_footer(out).signed)
        with self.assertRaises(pkg.PackageError):
            pkg.verify(out, {"dev": PUB})
        self.assertIsNone(pkg.verify(out, {}, allow_unsigned=True))

    def test_rejects(self):
        out = self.make()
        with self.assertRaises(pkg.PackageError):
            pkg.verify(out, {})  # unknown key
        raw = bytearray(out.read_bytes())
        raw[10] ^= 1
        bad = self.dir / "bad.aember"
        bad.write_bytes(bytes(raw))
        with self.assertRaises(pkg.PackageError):
            pkg.verify(bad, {"dev": PUB})  # payload changed
        raw = bytearray(out.read_bytes())
        raw[-150] ^= 1  # the digest: the signature no longer matches
        bad.write_bytes(bytes(raw))
        with self.assertRaises(pkg.PackageError):
            pkg.verify(bad, {"dev": PUB})
        bad.write_bytes(b"short")
        with self.assertRaises(pkg.PackageError):
            pkg.read_footer(bad)
        with self.assertRaises(pkg.PackageError):
            self.make(name="other.aember")  # the name must match

    def test_keys(self):
        private, public = pkg.keygen(self.dir / "keys", "dev")
        self.assertEqual(private.stat().st_mode & 0o777, 0o600)
        seed = pkg.read_private_key(private)
        self.assertEqual(pkg.read_public_key(public), ed25519.public_key(seed))
        self.assertEqual(
            pkg.load_keys(self.dir / "keys"), {"dev": ed25519.public_key(seed)}
        )
        self.assertEqual(pkg.load_keys(self.dir / "none"), {})
        with self.assertRaises(pkg.PackageError):
            pkg.keygen(self.dir / "keys", "dev")  # exists
        (self.dir / "keys" / "bad.pub").write_text("nope")
        with self.assertRaises(pkg.PackageError):
            pkg.load_keys(self.dir / "keys")
        self.assertEqual(
            pkg.public_key_text(PUB), base64.b64encode(PUB).decode() + "\n"
        )

    def test_container_config_from_an_old_entry(self):
        entry = {
            "name": "web",
            "type": "container",
            "container": {
                "squashfs": "/x",
                "rootfs": "/y",
                "config_path": "/z",
                "args": ["/bin/web", "-v"],
            },
            "init": True,
            "stop_timeout": 5,
            "restart_policy": "always",
            "dependencies": ["db"],
            "max_restart_attempts": 0,
            "restart_delay": 2,
        }
        self.assertEqual(
            pkg.container_config("web", entry),
            {
                "name": "web",
                "command": ["/bin/web", "-v"],
                "init": True,
                "stop_timeout": 5,
                "restart_policy": "always",
                "max_restart_attempts": 0,
                "restart_delay": 2,
            },
        )
        json.dumps(pkg.container_config("x", {}))


if __name__ == "__main__":
    unittest.main()
