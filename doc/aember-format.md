# The `.aember` package format (version 1)

One container in one file: its SquashFS image, how to start it
(`config.json`) and its LXC configuration (`lxc.container.conf`), compressed
and signed. A device unpacks a package **once**, when it is installed or
updated, onto its own disk, and starts the container from there on every
boot.

- Made by aember-build-helper (`aember-cli/run.py pack` in a container
  repository) or `aember package create`.
- Read on the device by `libraries/utils/package` (C++), used by
  aember-containerd.
- Python reader/writer: `aember-cli/aember/utils/aember_package.py` (the
  same module is in aember-build-helper).

## Layout

```
<payload>  one xz stream: a ustar tar of exactly
             config.json          how to start the container
             lxc.container.conf   LXC's own configuration, as LXC reads it
             image.squashfs       the root file system
           in this order, regular files, nothing else
<footer>   160 bytes
```

| Footer offset | Size | Content |
|---|---|---|
| 0 | 8 | `AEMBERv1` |
| 8 | 8 | payload size in bytes, little endian (= file size − 160) |
| 16 | 64 | SHA-512 of the payload |
| 80 | 8 | key id: the first 8 bytes of SHA-512 of the signing public key; zero = unsigned |
| 88 | 64 | Ed25519 (RFC 8032) signature of `"aember-package-v1\0"` followed by footer bytes 0–87; zero = unsigned |
| 152 | 8 | `1vREBMEA` |

The signature covers the payload through its SHA-512, so the device checks
the signature first (cheap, an unknown key stops there) and then hashes the
payload once.

Inspect a package with standard tools:

```bash
xz -dc --single-stream app.aember | tar tv     # the footer after the stream is ignored
aember package inspect app.aember --keys resources/x64-poky-linux/rootfs/etc/aember/keys
```

### Limits

`config.json` and `lxc.container.conf` up to 64 KiB each, the image up to
8 GiB. The xz decoder may use 128 MiB; packages are written with preset 6
(an 8 MiB dictionary, about 9 MiB of memory to unpack).

## config.json (a container)

```json
{
  "name": "aember-monitor",
  "command": ["/usr/bin/aember-monitor"],
  "init": true,
  "stop_timeout": 5,
  "restart_policy": "always",
  "max_restart_attempts": 0,
  "restart_delay": 2
}
```

`name` must be the package's file name without `.aember`:
`[a-z0-9][a-z0-9._-]*`, at most 64 characters, not ending in `.prev` or
`.new`, not `containers` or `packages`. The settings are those of a
container described in `doc/configuration.md`; `command` is what used to be
`container.args`. Which containers a board starts, and in which order, is
not in the package: that is the board's `containers.json`.

## Keys and signatures

Ed25519. A private key is a `.key` file (the 32-byte seed in base64, kept
off the device and out of git); its public key is a `.pub` file (32 bytes in
base64). `aember package keygen <name>` makes a pair.

A board trusts the `.pub` files in `/etc/aember/keys/`. A device accepts
an unsigned package only when its `containers.json` says
`"allow_unsigned": true`: development boards, never a product.

**Why the signature is in the file (not a detached `.sig`):** one file to
build, copy, upload and install; a package can't lose its signature on the
way. The footer is at the end so a package can be written in one pass
(the digest is known only after the payload).

## On the device

```
/var/lib/aember/packages/<name>.aember   a package to install (removed once installed)
/var/lib/aember/<name>/                  the installed package:
    config.json  lxc.container.conf  image.squashfs  package.json
/var/lib/aember/<name>.prev/             the version before (for a rollback)
/var/lib/aember/.<name>.new/             an unpack in progress
```

Install (`aember::utils::package::Install`):

1. Check the name, the signature against `/etc/aember/keys`, the SHA-512.
2. The same package is installed already (`package.json`'s digest): remove
   the file, done.
3. Unpack into `.<name>.new/`: each file written new and synced, then
   `config.json` checked (its name, containerd's parser), `package.json`
   (name, digest, signing key) written last, the directory synced.
4. Swap `.<name>.new` and `<name>` in one step (`renameat2`
   `RENAME_EXCHANGE`), the old version becomes `<name>.prev`, the directory
   is synced, the package file removed.

`<name>` is never missing: a power loss leaves the old or the new version.
`Recover` (at containerd's start) puts a missing `<name>` back and removes
unfinished unpacks. `Rollback` swaps `<name>` and `<name>.prev`.

## Size: what compression where

Measured with aember's containers (x64), bytes:

| | aember-echo | aember-stats | aember-monitor |
|---|---|---|---|
| tar of the three files (SquashFS xz, 128 KiB blocks) | 3,256,320 | 2,170,880 | 9,922,560 |
| that tar in xz -9e | 3,241,900 | 2,166,028 | 9,912,488 |
| … in zstd -19 | 3,240,599 | 2,164,767 | 9,911,024 |

The SquashFS image is compressed already, so compressing the package saves
only the tar's padding and the text files (0.1–0.5 %); xz and zstd are
equal here. The payload is xz (liblzma is in every SDK, and `xz` reads it
anywhere). The real saving is in the image itself (aember-monitor):

| SquashFS options | Image size |
|---|---|
| xz, 128 KiB blocks (before) | 9,916,416 |
| xz, 1 MiB blocks | 9,211,904 (−7 %) |
| xz, 1 MiB blocks, BCJ filter for the architecture, whole-block dictionary | 8,785,920 (−11 %) |
| zstd 19, 1 MiB blocks | 10,113,024 |

aember-build-helper builds images with the last xz settings.
