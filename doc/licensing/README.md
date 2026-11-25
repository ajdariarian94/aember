# Licensing

aember's own code is under the Aember License (`LICENSE` at the repository
root). This directory is about everything else that ends up on a device, and
the material for the legal review before aember is sold.

| File | What |
|---|---|
| [THIRD_PARTY.md](THIRD_PARTY.md) | Inventory: every third-party component an image can contain, licence, linking, obligation; blockers |
| [components.json](components.json) | The same, machine-readable; `aember app pack` writes `/usr/share/licenses/aember/NOTICES.gz` from it |
| [texts/](texts/) | Licence texts (SPDX, from Poky's common-licenses) that NOTICES carries |
| [SOURCE-OFFER.md](SOURCE-OFFER.md) | GPL/LGPL written source offer (template; vendor details per product) |
| [LICENSING-NOTES.md](LICENSING-NOTES.md) | Open questions for the lawyer |
| [CLA-DRAFT.md](CLA-DRAFT.md), [cla-workflow.yml.example](cla-workflow.yml.example) | Contributor licence agreement draft and its CLA Assistant workflow (not enabled) |

On a device: `zcat /usr/share/licenses/aember/NOTICES.gz`.
