# Monocypher 4.0.2 (vendored)

SHA-512 and Ed25519 (RFC 8032) for the signatures of `.aember` packages
(`libraries/utils/package`): `monocypher.c/.h` and the optional
`monocypher-ed25519.c/.h`, unmodified.

- Source: https://monocypher.org/download/monocypher-4.0.2.tar.gz
  (sha256 `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`),
  the same files aember-auth and aember-fleet vendor.
- Licence: CC0-1.0 or BSD-2-Clause, at our choice (`LICENCE.md`). We use it
  under BSD-2-Clause, so its notice goes into the image's third-party
  notices.
- Built as C++ (Monocypher compiles as either) inside the package library,
  without aember's warnings (it isn't our code); `aember analyze` and
  `aember lint` leave `third_party/` alone.

To update: replace the four source files and this README's version and hash
together.
