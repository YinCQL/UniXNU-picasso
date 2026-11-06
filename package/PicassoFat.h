/*
 * Copyright (c) 2026, UniXNU contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * Subject to the terms and conditions of this license, each copyright holder
 * and contributor hereby grants to those receiving rights under this license
 * a perpetual, worldwide, non-exclusive, no-charge, royalty-free, irrevocable
 * (except for failure to satisfy the conditions of this license) patent
 * license to make, have made, use, offer to sell, sell, import, and otherwise
 * transfer this software, where such license applies only to those patent
 * claims, already acquired or hereafter acquired, licensable by such copyright
 * holder or contributor that are necessarily infringed by:
 *
 * (a) their Contribution(s) (the licensed copyrights of copyright holders and
 *     non-copyrightable additions of contributors, in source or binary form)
 *     alone; or
 *
 * (b) combination of their Contribution(s) with the work of authorship to
 *     which such Contribution(s) was added by such copyright holder or
 *     contributor, if, at the time the Contribution is added, such addition
 *     causes such combination to be necessarily infringed. The patent license
 *     shall not apply to any other combinations which include the
 *     Contribution.
 *
 * Except as expressly stated above, no rights or licenses from any copyright
 * holder or contributor is granted under this license, whether expressly, by
 * implication, estoppel or otherwise.
 *
 * DISCLAIMER
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Adapted from UniXNU-sheng 2fa72907d90986339ba1677d262ef51655cee02e package/FatVolume.h.
 * Picasso supports the supplied stock FAT16 firmware only. */
/* A read-only Picasso FAT16 volume over a byte read callback: enough to find a file by its long or short (8.3) name in a directory path and read
 * any range of it. Used to take the device's own firmware files from its firmware partition in place. No allocation: the one sector of
 * the allocation table in use is cached in the volume. The volume's metadata is untrusted: every cluster number and chain is bounds
 * checked and every chain walk is bounded, so a damaged volume gives an error, never a loop or a read outside the partition. */
#ifndef PICASSO_FAT_H
#define PICASSO_FAT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Reads length bytes at offset (from the partition start) into to; returns 0 on success. */
typedef int (*FatRead)(void *context, uint64_t offset, void *to, size_t length);

enum {
    FatOk = 0,
    FatReadError = -1,
    FatNotFat = -2,     /* no boot sector of a FAT16 or FAT32 volume */
    FatNotFound = -3,
    FatNotDirectory = -4,
    FatIsDirectory = -5,
    FatDamaged = -6,    /* a cluster chain leaves the volume, loops or is shorter than the file */
    FatRange = -7,      /* the read goes past the end of the file */
};

typedef struct {
    FatRead read;
    void *context;
    uint64_t size;                                    /* partition bytes */
    uint8_t fat32;
    uint32_t sector_bytes, cluster_bytes, clusters;   /* clusters: data clusters, numbered 2 .. clusters + 1 */
    uint64_t fat_offset, root_offset, data_offset;    /* bytes from the partition start; root_offset is FAT16 only */
    uint32_t root_entries, root_cluster;              /* root_entries: FAT16 fixed root; root_cluster: FAT32 */
    uint64_t cached_sector;                           /* UINT64_MAX: nothing cached */
    uint8_t sector[4096];
} FatVolume;

typedef struct {
    uint32_t first_cluster, size;
    uint8_t directory;
} FatFile;

int picasso_fat_validate(FatVolume *v, const FatFile *file, uint8_t seen[8192]);
int picasso_fat_mount(FatVolume *v, uint64_t size, FatRead read, void *context);
/* path: names separated by '/', each matched in any case against the long name, else the 8.3 name ("image/adsp.b00"). */
int picasso_fat_open(FatVolume *v, const char *path, FatFile *file);
int picasso_fat_read(FatVolume *v, const FatFile *file, uint64_t offset, void *to, size_t length);

#ifdef __cplusplus
}
#endif
#endif
