# Third-party software on an aember device

What an aember image can contain besides aember's own code, under which
licence, how it is linked, and what we must do when we ship it. Versions are
those of aember-resources v0.1.15 (Yocto, meta-openembedded, meta-qt6 6.10),
aember's vcpkg baseline and the containers in `resources/*/rootfs/`.

The machine-readable copy is [`components.json`](components.json): `aember
app pack` writes the notices of the components an image really contains to
`/usr/share/licenses/aember/NOTICES.gz` (with each licence text once and the
[source offer](SOURCE-OFFER.md)). Open legal questions are in
[`LICENSING-NOTES.md`](LICENSING-NOTES.md). This is an engineering
inventory, not legal advice.

**Legend.** *Separate program*: a GPL program next to ours; that doesn't
make our code GPL, but its source must be offered. *Dynamic*: a shared
library our program loads; LGPL allows that for non-LGPL programs if the user
can replace the library. *Static*: compiled into our binary.

## Blockers and risks (summary)

| # | What | Why | Fix |
|---|---|---|---|
| 1 | **wolfSSL 5.8.0 (GPL-2.0-only) in aember-auth and aember-fleet-agent** | Our programs (Aember License) link a GPL-2.0-only library: distributing them as non-GPL is not allowed. | Code moved to Monocypher + mbedTLS backends (aember-auth #7, aember-fleet #8); the SDK still has to ship mbedTLS (phase 2). Until then: don't ship these containers commercially, or buy a commercial wolfSSL licence. |
| 2 | **GNU readline 8.3 (GPL-3.0-or-later) in connmanctl** (connman package) | GPLv3 on a consumer device ("User Product") requires the "Installation Information" to install a modified version: conflicts with a locked device. | Drop `connman-client` from the package (ConnMan is controlled over D-Bus anyway), phase 2. |
| 3 | **Qt 6 (LGPL-3.0-only) in aember-monitor** | Same LGPLv3/GPLv3 installation rule: the device owner must be able to replace Qt. Signed-only fleet updates or secure boot would block that. | Keep devices open for owners (documented way to install own images/keys), or buy a Qt commercial licence. Lawyer question. |
| 4 | GPL/LGPL source offer (kernel, BusyBox, liblxc + our patch, glibc, ConnMan, e2fsprogs, wolfSSL in Wi-Fi, ...) | Every shipped binary needs the corresponding source (incl. our patches and configs) for 3 years. | Written offer (SOURCE-OFFER.md, on the image); phase 2 publishes the source archives per release. |
| 5 | Attribution for MIT/BSD/FTL code (nlohmann/json, spdlog, Monocypher, FreeType, ...) | Notices must travel with binaries. | Done: NOTICES.gz on every image. |

Nothing else found that forbids selling aember under its own licence.

## Base image (every board)

| Component | Version | Licence | Linking | Obligation |
|---|---|---|---|---|
| Linux kernel | 6.16 | GPL-2.0-only (syscall note) | separate (kernel) | source + our config fragments (aember-resources `meta-aember/recipes/kernel/files`) |
| BusyBox | 1.37.0 | GPL-2.0-only, bzip2-1.0.6 | separate program | source + `.config` |
| glibc (glibc boards) | 2.42 | LGPL-2.1-or-later | dynamic | source; replaceable (it is: shared library) |
| musl (`-musl` boards) | 1.2.5 | MIT | dynamic | notice |
| GCC runtime: libstdc++, libgcc_s | 15.2 | GPL-3.0 + GCC Runtime Library Exception | dynamic | notice; the exception covers us as long as we compile with an unmodified GCC |
| liblxc | 6.0.2 (+ prctl fallback patch) | LGPL-2.1-only | dynamic (aember-containerd) | source incl. our patch; replaceable |
| nlohmann/json | 3.12.0 | MIT | header-only, in aember | notice |
| spdlog | 1.15.3 | MIT | static, in aember | notice |
| {fmt} | 11.0.2 | MIT (binary exception) | static, via spdlog | none for binaries; notice kept |
| GoogleTest | 1.17.0 | BSD-3-Clause | static, test binaries only | notice (Debug/test images) |

## Opt-in packages (`packages.json`, built by aember-resources)

| Package | Contents (version) | Licences |
|---|---|---|
| `connman` | ConnMan 1.44 (GPL-2.0-only), **connmanctl + readline 8.3 (GPL-3.0-or-later)**, D-Bus 1.16.2 (AFL-2.1 or GPL-2.0-or-later), GLib 2.84.2 (LGPL-2.1+), libffi 3.5.1 (MIT), PCRE2 10.45 (BSD-3), Expat 2.7.1 (MIT), libmnl 1.0.5 (LGPL-2.1+), libnftnl 1.2.9 (GPL-2.0+), libblkid/libmount 2.41.1 (LGPL-2.1+), ncurses libtinfo 6.5 (MIT), zlib 1.3.1 | GPL programs: source offer. readline: see blocker 2. |
| `dropbear` | Dropbear 2025.88 (MIT), libxcrypt 4.4.38 (LGPL-2.1), zlib | notices; libxcrypt source |
| `e2fsprogs` | e2fsck/mke2fs/resize2fs/tune2fs 1.47.3 (GPL-2.0-only), libext2fs (LGPL-2.0), libcom_err/libe2p (MIT), libblkid/libuuid (LGPL-2.1+/BSD-3) | source offer |
| `wifi` | wpa_supplicant 2.11 (BSD-3), **wolfSSL 5.8.0 (GPL-2.0-only)**, iw 6.9 (ISC), libnl 3.11.0 (LGPL-2.1), wireless-regdb (ISC), Pi 4: BCM43455 firmware (Broadcom bcm43xx firmware licence) | wolfSSL here is fine licence-wise (wpa_supplicant is BSD, GPL-compatible, and a separate program from ours): source offer. Firmware: its licence file goes with it. |
| `wifi-ap` | hostapd 2.11 (BSD-3), wolfSSL 5.8.0 (GPL-2.0-only), libnl | as `wifi` |

hostapd/wpa_supplicant 2.11 have no mbedTLS backend (only OpenSSL, GnuTLS,
wolfSSL, internal). Keeping wolfSSL there is allowed (source offer); a
customer that wants no GPL in the Wi-Fi stack gets OpenSSL 3 (Apache-2.0,
~4 MB more) as a build option, phase 2.

## Containers aember ships in its board overlays

| Container | Third-party contents | Licences |
|---|---|---|
| `aember-echo` / `alpine-echo` | Alpine Linux 3.20.0 minirootfs (BusyBox, musl, apk-tools, ...) | GPL-2.0-only, MIT, others (`lib/apk/db/installed` in the image). **Removed** from aember's boards (2026-11-27); the entry stays for images that add Alpine containers. |
| `aember-stats` | glibc/musl, GCC runtime | ours + as base image |
| `aember-monitor` | Qt 6.10 qtbase (Core, Gui, Network; linuxfb, offscreen), FreeType (FTL), libpng (Libpng), PCRE2-16 (BSD-3), zlib, glibc, GCC runtime | Qt: LGPL-3.0-only (blocker 3), the rest notices |
| `aember-auth` | wolfSSL 5.8.0 → mbedTLS 3.6 (Apache-2.0) in phase 2, Monocypher 4.0.2 (BSD-2/CC0, after #7), nlohmann/json (MIT) | blocker 1 |
| `aember-fleet-agent` | same as aember-auth (SHA-256 + Ed25519 only, no TLS) | blocker 1 |

## Boot files (SD card / USB images only)

| Component | Licence | Notes |
|---|---|---|
| Raspberry Pi GPU firmware `start4.elf`, `fixup4.dat` (raspberrypi/firmware 1.20260915) | Broadcom's `LICENCE.broadcom` (redistribution allowed, only for use with Raspberry Pi devices, unmodified) | the licence file goes on the boot partition (it is downloaded with them) |
| systemd-boot 255.4 (`aember usb image`) | LGPL-2.1-or-later | source offer (Ubuntu's source package) |

## Outside the device

- **aember-fleet server** (Docker image, `server/`): Python 3.12 (PSF-2.0) on
  `python:3.12-slim` (Debian: many licences, GPL included), FastAPI 0.115.6
  (MIT), Uvicorn 0.34.0 (BSD-3), cryptography 44.0.0 (Apache-2.0 or BSD-3,
  bundles OpenSSL 3, Apache-2.0), Starlette, Pydantic, AnyIO, h11, click,
  idna (MIT/BSD). No AGPL: running it as a service has no source
  obligations; shipping the image to customers means the Debian base's
  source offer (Debian publishes it) and the notices.
- **Dev images on GHCR** (`aember-dev`, `aember-website-dev`,
  `aember-resources-dev`): public redistributions of Ubuntu and the SDK
  (GCC, binutils: GPL). Source availability is upstream's (Ubuntu, Yocto
  mirrors), see LICENSING-NOTES.md.
- **Build tools** (Yocto, GCC, QEMU, Compiler Explorer, kas): used, not
  shipped; their licences don't reach what they build (GCC: runtime
  exception above).
- **Website** (aember-website): React and friends (MIT), bundled in the
  static site; their notices are in the bundle's licence comments.

## Keeping this current

- A new dependency on the device: add it to `components.json` (and this
  file) in the same PR; `aember-cli/tests/test_notices.py` checks the file.
- Licence texts in `texts/` are the SPDX texts from Poky's
  `meta/files/common-licenses`; add the one a new licence needs.
- Each release: phase 2 adds Yocto's SPDX SBOM and source archives
  (`INHERIT += "archiver create-spdx"`), which replace hand-kept versions
  for everything aember-resources builds.
