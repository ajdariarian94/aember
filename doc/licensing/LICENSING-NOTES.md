# Licensing notes for legal review

Written by the engineering side (2026-11-26) for the lawyer who reviews
aember's licensing before it is sold. Facts about the code are in
[THIRD_PARTY.md](THIRD_PARTY.md); these are the questions we can't answer
ourselves. Nothing here is legal advice.

## Context

- aember is a Linux init/container system for embedded devices. The owner
  (Arian Ajdari) wants to sell commercial licences; personal, educational and
  evaluation use stays free.
- All repositories carry the same custom "Aember License" (`LICENSE`):
  source-available, non-commercial use free, any company/commercial use needs
  a paid licence, each version converts to Apache-2.0 after a Change Date.
- Products built with aember are devices (consumer and industrial) whose
  firmware contains aember, third-party GPL/LGPL components and the
  customer's own containers; updates may come over the air through
  aember-fleet with signed releases.

## Questions

1. **wolfSSL (GPL-2.0-only) linked by aember-auth / aember-fleet-agent.**
   Engineering is replacing it with mbedTLS (Apache-2.0) and Monocypher
   (BSD-2/CC0). Until then: is shipping these two containers in an evaluation
   (free, non-commercial) image acceptable, given they would have to be
   offered under GPL-2.0 terms? Is there exposure from versions already
   published on GitHub (source only; binaries in release tarballs)?
2. **LGPL-3.0 (Qt) and GPL-3.0 (readline) on locked devices.** If a customer
   locks the device (secure boot, or only release-key-signed updates through
   aember-fleet), does that breach LGPLv3/GPLv3's Installation Information
   requirement for "User Products"? Is it enough to document how an owner
   installs their own release key / image? Should aember's commercial terms
   oblige customers to keep this possible, or to obtain a Qt commercial
   licence? (Engineering plans to drop readline/connmanctl.)
3. **Source offer.** Who is the distributor that owes the GPL/LGPL source
   offer for a customer's device: aember's vendor, the customer, or both? Is
   the written offer in [SOURCE-OFFER.md](SOURCE-OFFER.md) (on every image, in
   `/usr/share/licenses/aember/NOTICES.gz`) adequate, or must source ship with
   each device or release? Does the customer contract need a clause passing
   this duty on?
4. **Aember License, Change Date wording.** The Change Date section says
   (a) each version converts on 16 August 2030 and (b) versions released after
   that date convert five years after their own release. Read together, it
   is unclear whether a version released in 2028 converts in 2030 or 2033.
   Recommend wording per version ("four/five years after the version's first
   public release"), as in the Business Source License 1.1.
5. **Evaluation by companies.** The licence lists "evaluation, testing and
   non-production" as free, but also says any use by or for a company needs a
   commercial licence. Can a company evaluate for free? Suggest stating it
   (e.g. "evaluation by a company for up to 90 days, not in production").
6. **Standard vs custom licence.** Would BSL 1.1 (with an Additional Use
   Grant carrying today's free uses) or PolyForm Noncommercial 1.0 + a
   separate commercial licence express the same intent with less review
   friction for customers' legal departments?
7. **Contributions.** The licence's contribution clause asks contributors to
   allow relicensing. Is that enforceable without a signed agreement? A
   draft CLA is in [CLA-DRAFT.md](CLA-DRAFT.md) (CLA Assistant can collect
   signatures on GitHub; not enabled).
8. **AI-assisted authorship.** Much of aember's code was written with an AI
   assistant (Claude) under the owner's direction, review and design;
   commits carry a `Co-Authored-By: Claude` line. What does that mean for the
   copyright the licence relies on (EU/Germany and US), and should the
   documentation or contracts address it?
9. **Liability and warranty.** The licence disclaims everything ("AS IS").
   Under German law (BGB §§ 307 ff., Produkthaftungsgesetz, and the EU Cyber
   Resilience Act for products with digital elements) such disclaimers are
   limited, especially for paying customers. What should the commercial
   licence say instead (cap, exclusions, security update duties)?
10. **Distribution of public dev images.** The Docker images on GHCR
    (`aember-dev` etc.) redistribute Ubuntu packages and the Yocto SDK (GCC,
    binutils: GPL). Is pointing to upstream sources sufficient, or should we
    publish the corresponding source too?
11. **Third-party firmware.** Raspberry Pi GPU firmware (`LICENCE.broadcom`,
    use with Raspberry Pi devices only) and the Broadcom BCM43455 Wi-Fi
    firmware licence: any conditions for a commercial product built on a
    Raspberry Pi 4 / CM4?
12. **Trademark.** "aember" as an EU trademark (classes 9 and 42): conflicts?

## What engineering has done / will do

- Inventory and notices on every image (THIRD_PARTY.md, components.json,
  NOTICES.gz).
- wolfSSL out of our own programs (code done; SDK switch in aember-resources
  v0.1.16).
- Phase 2: per-release SBOM (SPDX) and source archives from Yocto
  (`archiver`, `create-spdx`), readline/connmanctl dropped, optional OpenSSL
  backend for the Wi-Fi stack.
- The LICENSE files are unchanged: changing them is the owner's decision
  after this review.
