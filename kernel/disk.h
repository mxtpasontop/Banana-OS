/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISK_H
#define DISK_H

#include "types.h"
#include "ata.h"

struct blockdev;

/* Disks of every kind - IDE (ata.c, atapi.c), SATA (ahci.c), NVMe (nvme.c) - behind
 * one interface, for install/sync and the disk listing. */

#define DISK_MAX 20

/* every present drive (hard disks and CD/DVD drives); returns the count */
int disk_probe(ata_disk_t* out, int max);

/* 512-byte sectors; 0 = ok */
int disk_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf);
int disk_write(const ata_disk_t* d, uint32_t lba, uint32_t count, const void* buf);
int disk_flush(const ata_disk_t* d);
/* bytes read and written by every disk since boot, and the nanoseconds spent on them (Task Manager) */
void disk_io_stats(uint64_t* rd, uint64_t* wr, uint64_t* busy_ns);

/* CD/DVD: 2048-byte blocks, and the size of the ISO9660 image on it */
int disk_cd_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf);
int disk_cd_iso_size(const ata_disk_t* d, uint32_t* out_bytes);

/* 1 if a and b are the same drive */
int  disk_same(const ata_disk_t* a, const ata_disk_t* b);
/* the block device a hard disk is behind (NVMe, virtio, extra IDE / SATA), or NULL */
struct blockdev* disk_blockdev(const ata_disk_t* d);
/* the IDE and SATA hard disks become block devices (their volumes mounted);
 * the ones holds_system says are Banana OS's are marked never to be mounted */
void disk_attach_extra(int (*holds_system)(const ata_disk_t* d));

/* "IDE primary master" / "SATA port 2" */
void disk_describe(const ata_disk_t* d, char* out, int cap);

#endif
