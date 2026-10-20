/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKPART_H
#define DISKPART_H

#include "types.h"
#include "blockdev.h"
#include "volume.h"

/*
 * Disk Management's work (the window: kernel/diskmgr.c; the shell:
 * `diskmgr`): the disks with their partitions, the free space between them
 * and what each volume holds, and the changes - a new partition table (MBR
 * or GPT), a new volume in free space, formatting as FAT32 or exFAT,
 * deleting a partition, mounting and unmounting. The disk Banana OS runs
 * from and CD drives are shown, never changed.
 */

#define DP_MAX_DISKS 16
#define DP_MAX_PARTS 16
#define DP_MAX_GAPS  8

enum { DP_NONE = 0, DP_MBR, DP_GPT };            /* the partition table */
enum { DP_FAT32 = 1, DP_EXFAT };                 /* what a volume can be formatted as */

typedef struct {
    int      number;             /* 1.. (0: the whole disk, no table) */
    uint32_t base, sectors;
    char     type[24];           /* the table's word for it: "FAT32 (LBA)", "Microsoft basic data" */
    char     fs[12];             /* what is in it: "FAT32", "exFAT", "NTFS", "ext4", "" (unknown / empty) */
    char     label[24];
    int      mnt;                /* its mount id while mounted, else 0 */
    char     where[48];          /* its mount point, "/" for Banana OS */
    uint64_t total, free_bytes;  /* while mounted (0: unknown) */
    int      system;             /* Banana OS's own */
} dp_part_t;

typedef struct {
    char        name[16];        /* "sata1", "usb0", "disk0" */
    char        model[40];
    char        kind[16];        /* "Basic", "Removable", "CD-ROM" */
    uint32_t    sectors;
    int         system, cdrom, removable;
    int         writable;        /* a drive Disk Management may change */
    int         scheme;          /* DP_* */
    blockdev_t* bd;              /* NULL: not one of the changeable drives */
    int         nparts;
    dp_part_t   parts[DP_MAX_PARTS];
    int         ngaps;           /* unallocated space, aligned to 1 MiB */
    uint32_t    gap_base[DP_MAX_GAPS], gap_sectors[DP_MAX_GAPS];
} dp_disk_t;

/* every disk: how many */
int  dp_scan(dp_disk_t* out, int max);
/* the same disk read again (after a change) */
int  dp_rescan(dp_disk_t* d);
/* "300 MB", "1.5 GB" */
void dp_size_text(uint64_t bytes, char* out, int cap);

/* the changes: 0, or -1 with err. Each unmounts the disk's volumes first
 * and mounts what there is afterwards. */
int  dp_init(dp_disk_t* d, int scheme, char* err, int ecap);                 /* a new, empty table */
/* a partition in free space (gap: which unallocated area; sectors 0: all of
 * it), formatted as fs with label; its number in *number */
int  dp_create(dp_disk_t* d, int gap, uint32_t sectors, int fs, const char* label, int* number, char* err, int ecap);
int  dp_delete(dp_disk_t* d, int number, char* err, int ecap);
/* number 0: the whole disk without a table (a "superfloppy") */
int  dp_format(dp_disk_t* d, int number, int fs, const char* label, char* err, int ecap);
int  dp_mount(dp_disk_t* d, int number, char* err, int ecap);
int  dp_unmount(dp_disk_t* d, int number, char* err, int ecap);
/* the part by its number, NULL */
dp_part_t* dp_part(dp_disk_t* d, int number);
/* "usb0p2" / "usb0" -> the disk and part number (-1 if no part given) */
dp_disk_t* dp_find(dp_disk_t* disks, int n, const char* name, int* number);

#endif
