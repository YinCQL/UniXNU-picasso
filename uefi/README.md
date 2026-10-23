# uefi/

Place the compiled picasso firmware volume here:

    uefi/SILICIUM_UEFI.fd

Get it by running the "Build and upload picasso FD" workflow in
YinCQL/Mu-Silicium (branch UniXNU) and downloading the picasso-uefi-fd
artifact, or build locally and copy:

    Build/picassoPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd -> uefi/SILICIUM_UEFI.fd

The compose-bootimg workflow reads this file and packs it with the CI-built
kernel + payload into a flashable boot image.
