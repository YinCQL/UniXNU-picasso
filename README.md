# UniXNU-picasso

Redmi K30 5G (picasso, SM7250) device repository for UniXNU.

This repository holds the board-level files that adapt the generic UniXNU stack to the
picasso board: board description, kernel defconfig, ADT generator and the package hooks.
The UEFI firmware itself lives in the picasso Mu-Silicium port (YinCQL/Mu-Silicium,
Platforms/Xiaomi/picassoPkg).

Status: minimal skeleton. firmware step config present; kernel/IPSW/UserData are placeholders
to be filled during bring-up.
