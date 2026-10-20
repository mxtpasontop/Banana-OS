/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diskpart.h"
#include "disk.h"
#include "ata.h"
#include "fsdisk.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"
#include "random.h"
#include "deflate.h"

#define SEC        512u
#define ALIGN      2048u              /* partitions start on 1 MiB */
#define GPT_ENTS   128u
#define GPT_ESZ    128u
#define GPT_SECS   (GPT_ENTS * GPT_ESZ / SEC)   /* 32 */
#define CHUNK      64u                /* sectors written at a time */

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

static void fail(char* err, int ecap, const char* m) { if (err) kstrlcpy(err, m, (size_t)ecap); }

void dp_size_text(uint64_t b, char* out, int cap) {
    if (b >= ((uint64_t)1 << 40)) ksnprintf(out, (size_t)cap, "%u.%u TB", (uint32_t)(b >> 40), (uint32_t)(((b >> 30) & 1023) * 10 / 1024));
    else if (b >= ((uint64_t)1 << 30)) ksnprintf(out, (size_t)cap, "%u.%u GB", (uint32_t)(b >> 30), (uint32_t)(((b >> 20) & 1023) * 10 / 1024));
    else if (b >= ((uint64_t)1 << 20)) ksnprintf(out, (size_t)cap, "%u MB", (uint32_t)(b >> 20));
    else ksnprintf(out, (size_t)cap, "%u KB", (uint32_t)(b >> 10));
}

/* ── reading the disks that are no blockdev (the system's IDE disk): a stand-in ── */
static int ata_bd_read(blockdev_t* bd, uint32_t lba, uint32_t count, void* buf) {
    return disk_read((const ata_disk_t*)bd->priv, lba, count, buf);
}

/* what a volume holds, from its first sectors */
static void sniff(blockdev_t* bd, dp_part_t* p, uint8_t* b) {
    p->fs[0] = 0;
    p->label[0] = 0;
    if (bd->read(bd, p->base, 1, b) != 0) return;
    if (!memcmp(b + 3, "EXFAT   ", 8)) {
        kstrlcpy(p->fs, "exFAT", sizeof(p->fs));
        /* the label: an entry of the root folder's first sector */
        uint32_t heap = rd32(b + 88), root = rd32(b + 96), shift = b[109];
        if (shift > 16 || root < 2 || bd->read(bd, p->base + heap + ((root - 2) << shift), 1, b) != 0) return;
        for (int e = 0; e < 16; e++) {
            const uint8_t* d = b + e * 32;
            if (!d[0]) break;
            if (d[0] != 0x83) continue;
            int n = d[1] > 11 ? 11 : d[1], k = 0;
            for (int i = 0; i < n; i++) { uint16_t u = rd16(d + 2 + i * 2); p->label[k++] = u < 128 ? (char)u : '?'; }
            p->label[k] = 0;
            break;
        }
        return;
    }
    if (!memcmp(b + 3, "NTFS    ", 8)) { kstrlcpy(p->fs, "NTFS", sizeof(p->fs)); return; }
    if (b[510] == 0x55 && b[511] == 0xAA && !memcmp(b + 82, "FAT32   ", 8)) {
        kstrlcpy(p->fs, "FAT32", sizeof(p->fs));
        char l[12];
        memcpy(l, b + 71, 11);
        l[11] = 0;
        for (int i = 10; i >= 0 && l[i] == ' '; i--) l[i] = 0;
        if (strcmp(l, "NO NAME")) kstrlcpy(p->label, l, sizeof(p->label));
        return;
    }
    if (b[510] == 0x55 && b[511] == 0xAA && (!memcmp(b + 54, "FAT16   ", 8) || !memcmp(b + 54, "FAT12   ", 8))) {
        memcpy(p->fs, b + 54, 5); p->fs[5] = 0; return;
    }
    if (p->sectors > 4 && bd->read(bd, p->base + 2, 1, b) == 0 && rd16(b + 56) == 0xEF53) {
        uint32_t incompat = rd32(b + 96);
        kstrlcpy(p->fs, incompat & 0x40 ? "ext4" : (rd32(b + 92) & 4) ? "ext3" : "ext2", sizeof(p->fs));
        char l[17];
        memcpy(l, b + 120, 16);
        l[16] = 0;
        kstrlcpy(p->label, l, sizeof(p->label));
        return;
    }
    if (p->sectors > 8 && bd->read(bd, p->base + 7, 1, b) == 0 && !memcmp(b + 502, "SWAPSPACE2", 10)) kstrlcpy(p->fs, "swap", sizeof(p->fs));
}

static void find_gaps(dp_disk_t* d) {
    d->ngaps = 0;
    if (d->scheme == DP_NONE && d->nparts) return;              /* a superfloppy: no room */
    uint32_t lo = ALIGN, hi = d->scheme == DP_GPT ? d->sectors - 1 - GPT_SECS : d->sectors;
    if (d->sectors < 2 * ALIGN) return;
    /* the partitions in order of where they start */
    int order[DP_MAX_PARTS];
    for (int i = 0; i < d->nparts; i++) order[i] = i;
    for (int i = 1; i < d->nparts; i++)
        for (int j = i; j > 0 && d->parts[order[j]].base < d->parts[order[j - 1]].base; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    uint32_t at = lo;
    for (int k = 0; k <= d->nparts && d->ngaps < DP_MAX_GAPS; k++) {
        uint32_t end = k < d->nparts ? d->parts[order[k]].base : hi;
        uint32_t start = (at + ALIGN - 1) / ALIGN * ALIGN;
        if (end > start && end - start >= ALIGN) {
            d->gap_base[d->ngaps] = start;
            d->gap_sectors[d->ngaps] = end - start;
            d->ngaps++;
        }
        if (k < d->nparts) {
            uint32_t pe = d->parts[order[k]].base + d->parts[order[k]].sectors;
            if (pe > at) at = pe;
        }
    }
}

static void fill(dp_disk_t* d, blockdev_t* bd) {
    uint8_t* b = (uint8_t*)kmalloc(SEC);
    vol_part_t* vp = (vol_part_t*)kmalloc(sizeof(vol_part_t) * VOL_MAX_PARTS);
    d->nparts = 0;
    d->scheme = DP_NONE;
    if (!b || !vp || d->cdrom) { kfree(b); kfree(vp); return; }
    int gpt = 0;
    int n = volume_partitions(bd, vp, VOL_MAX_PARTS, &gpt);
    if (n > 0 || gpt) d->scheme = gpt ? DP_GPT : DP_MBR;
    else if (bd->read(bd, 0, 1, b) == 0 && b[510] == 0x55 && b[511] == 0xAA && memcmp(b + 82, "FAT32   ", 8) &&
             memcmp(b + 3, "EXFAT   ", 8) && memcmp(b + 3, "NTFS    ", 8)) d->scheme = DP_MBR;   /* an empty MBR */
    for (int i = 0; i < n && d->nparts < DP_MAX_PARTS; i++) {
        dp_part_t* p = &d->parts[d->nparts++];
        memset(p, 0, sizeof(*p));
        p->number = vp[i].number;
        p->base = vp[i].base;
        p->sectors = vp[i].sectors;
        kstrlcpy(p->type, vp[i].type, sizeof(p->type));
        sniff(bd, p, b);
        if (vp[i].label[0] && !p->label[0]) kstrlcpy(p->label, vp[i].label, sizeof(p->label));
    }
    if (d->scheme == DP_NONE) {
        /* no table: a volume on the whole disk? */
        dp_part_t t;
        memset(&t, 0, sizeof(t));
        t.sectors = d->sectors;
        sniff(bd, &t, b);
        if (t.fs[0]) { d->parts[0] = t; kstrlcpy(d->parts[0].type, "whole disk", sizeof(d->parts[0].type)); d->nparts = 1; }
    }
    /* mounted ones */
    for (int i = 0; i < d->nparts; i++) {
        dp_part_t* p = &d->parts[i];
        if (d->system) {
            p->system = 1;
            if (!strcmp(p->fs, "ext4") || (!p->fs[0] && (!strcmp(p->type, "Linux") || !strcmp(p->type, "Linux filesystem")))) {
                kstrlcpy(p->where, "/", sizeof(p->where));
                if (!p->fs[0]) kstrlcpy(p->fs, fsdisk_fs_type(), sizeof(p->fs));
                uint64_t used = 0, cap = 0;
                fsdisk_space(&used, &cap);
                p->total = cap;
                p->free_bytes = cap > used ? cap - used : 0;
            }
            continue;
        }
        if (!d->bd) continue;
        for (int v = 0; v < d->bd->nvol; v++)
            if (d->bd->vol_base[v] == p->base) {
                p->mnt = d->bd->vol_mnt[v];
                kstrlcpy(p->where, fs_mount_point(p->mnt), sizeof(p->where));
                volume_space(p->mnt, &p->total, &p->free_bytes);
            }
    }
    find_gaps(d);
    kfree(b);
    kfree(vp);
}

int dp_rescan(dp_disk_t* d) {
    if (d->bd) { fill(d, d->bd); return 0; }
    return -1;
}

int dp_scan(dp_disk_t* out, int max) {
    int n = 0;
    ata_disk_t* all = (ata_disk_t*)kmalloc(sizeof(ata_disk_t) * DISK_MAX);
    if (!all) return 0;
    int na = disk_probe(all, DISK_MAX);
    ata_disk_t inst;
    int have_inst = fsdisk_is_installed() && fsdisk_find_installed(&inst);
    blockdev_t* seen[DISK_MAX];
    int nseen = 0, ndisk = 0, ncd = 0;
    for (int i = 0; i < na && n < max; i++) {
        ata_disk_t* a = &all[i];
        dp_disk_t* d = &out[n++];
        memset(d, 0, sizeof(*d));
        blockdev_t* bd = disk_blockdev(a);
        if (bd && nseen < DISK_MAX) seen[nseen++] = bd;
        d->cdrom = a->is_atapi;
        d->system = !a->is_atapi && ((have_inst && disk_same(a, &inst)) || (bd && bd->system));
        d->sectors = a->sectors;
        kstrlcpy(d->model, a->model[0] ? a->model : (a->is_atapi ? "CD drive" : "Disk"), sizeof(d->model));
        if (a->is_atapi) { ksnprintf(d->name, sizeof(d->name), "cd%d", ncd++); kstrlcpy(d->kind, "CD-ROM", sizeof(d->kind)); continue; }
        if (bd) kstrlcpy(d->name, bd->name, sizeof(d->name));
        else ksnprintf(d->name, sizeof(d->name), "disk%d", ndisk);
        ndisk++;
        kstrlcpy(d->kind, "Basic", sizeof(d->kind));
        d->bd = bd;
        d->writable = bd && !d->system && bd->write && (bd->sector_size == 0 || bd->sector_size == SEC);
        if (bd) fill(d, bd);
        else {
            blockdev_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.present = 1;
            tmp.sectors = a->sectors;
            tmp.read = ata_bd_read;
            tmp.priv = a;
            fill(d, &tmp);
        }
    }
    for (int i = 0; i < BLOCKDEV_MAX && n < max; i++) {
        blockdev_t* bd = blockdev_get(i);
        if (!bd || !bd->present) continue;
        int dup = 0;
        for (int k = 0; k < nseen; k++) if (seen[k] == bd) dup = 1;
        if (dup) continue;
        dp_disk_t* d = &out[n++];
        memset(d, 0, sizeof(*d));
        kstrlcpy(d->name, bd->name, sizeof(d->name));
        kstrlcpy(d->model, bd->model[0] ? bd->model : bd->name, sizeof(d->model));
        d->removable = !strcmp(bd->kind, "usb") || !strcmp(bd->kind, "sd");
        kstrlcpy(d->kind, d->removable ? "Removable" : "Basic", sizeof(d->kind));
        d->sectors = bd->sectors;
        d->system = bd->system;
        d->bd = bd;
        d->writable = !bd->system && bd->write && (bd->sector_size == 0 || bd->sector_size == SEC);
        fill(d, bd);
    }
    kfree(all);
    return n;
}

dp_part_t* dp_part(dp_disk_t* d, int number) {
    for (int i = 0; i < d->nparts; i++) if (d->parts[i].number == number) return &d->parts[i];
    return NULL;
}

dp_disk_t* dp_find(dp_disk_t* disks, int n, const char* name, int* number) {
    if (number) *number = -1;
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(disks[i].name);
        if (strncmp(name, disks[i].name, (size_t)l)) continue;
        const char* rest = name + l;
        if (!*rest) return &disks[i];
        if (*rest == 'p' && rest[1] >= '0' && rest[1] <= '9') {
            uint32_t v;
            int k = k_parse_u32(rest + 1, &v);
            if (k > 0 && !rest[1 + k]) { if (number) *number = (int)v; return &disks[i]; }
        }
    }
    return NULL;
}

/* ── writing ── */

static int wr(blockdev_t* bd, uint32_t lba, uint32_t n, const void* buf) { return bd->write(bd, lba, n, buf); }

/* count sectors of zeros from lba */
static int zero(blockdev_t* bd, uint32_t lba, uint32_t count) {
    uint8_t* z = (uint8_t*)kmalloc(CHUNK * SEC);
    if (!z) return -1;
    memset(z, 0, CHUNK * SEC);
    int r = 0;
    while (count && !r) {
        uint32_t k = count > CHUNK ? CHUNK : count;
        r = wr(bd, lba, k, z);
        lba += k;
        count -= k;
    }
    kfree(z);
    return r;
}

static void release(dp_disk_t* d) { if (d->bd) blockdev_eject_all(d->bd); }

static void remount(dp_disk_t* d) {
    if (!d->bd) return;
    char e[96];
    d->bd->mount_state = 2;                      /* (mounted here, not by the automount task) */
    if (volume_mount_all(d->bd, e, sizeof(e)) <= 0) d->bd->mount_state = -1;
    fill(d, d->bd);
}

static int check_writable(dp_disk_t* d, char* err, int ecap) {
    if (d->cdrom) { fail(err, ecap, "a CD cannot be changed"); return -1; }
    if (d->system) { fail(err, ecap, "that is the disk Banana OS runs from"); return -1; }
    if (!d->writable || !d->bd || !d->bd->present) { fail(err, ecap, "this disk cannot be changed here"); return -1; }
    return 0;
}

static void guid(uint8_t* g) {
    random_bytes(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);           /* version 4 */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
}

static const uint8_t GUID_BASIC_DATA[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };

/* the protective MBR of a GPT disk, or an empty MBR */
static void mbr_blank(uint8_t* b, uint32_t sectors, int gpt) {
    memset(b, 0, SEC);
    if (gpt) {
        uint8_t* e = b + 446;
        e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;          /* CHS 0/0/2 */
        e[4] = 0xEE;
        e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;
        wr32(e + 8, 1);
        wr32(e + 12, sectors - 1);
    } else {
        uint32_t id = random_u32();
        wr32(b + 440, id ? id : 0x42414E41u);
    }
    b[510] = 0x55;
    b[511] = 0xAA;
}

/* writes a GPT's headers and entries (ents: GPT_SECS sectors) */
static int gpt_write(blockdev_t* bd, uint32_t sectors, uint8_t* ents, const uint8_t* disk_guid) {
    uint8_t* h = (uint8_t*)kmalloc(SEC);
    if (!h) return -1;
    uint32_t last = sectors - 1, ecrc = crc32_update(0, ents, GPT_SECS * SEC);
    for (int backup = 0; backup < 2; backup++) {
        memset(h, 0, SEC);
        memcpy(h, "EFI PART", 8);
        wr32(h + 8, 0x00010000);
        wr32(h + 12, 92);
        wr64(h + 24, backup ? last : 1);
        wr64(h + 32, backup ? 1 : last);
        wr64(h + 40, 2 + GPT_SECS);
        wr64(h + 48, last - 1 - GPT_SECS);
        memcpy(h + 56, disk_guid, 16);
        wr64(h + 72, backup ? last - GPT_SECS : 2);
        wr32(h + 80, GPT_ENTS);
        wr32(h + 84, GPT_ESZ);
        wr32(h + 88, ecrc);
        wr32(h + 16, crc32_update(0, h, 92));
        if (wr(bd, backup ? last - GPT_SECS : 2, GPT_SECS, ents) || wr(bd, backup ? last : 1, 1, h)) { kfree(h); return -1; }
    }
    kfree(h);
    return 0;
}

int dp_init(dp_disk_t* d, int scheme, char* err, int ecap) {
    if (check_writable(d, err, ecap)) return -1;
    if (scheme == DP_MBR && d->sectors > 0xFFFFFFFEu) { fail(err, ecap, "too big for MBR: use GPT"); return -1; }
    if (d->sectors < 4 * ALIGN) { fail(err, ecap, "the disk is too small"); return -1; }
    release(d);
    uint8_t* b = (uint8_t*)kmalloc(SEC * (GPT_SECS + 2));
    if (!b) { fail(err, ecap, "out of memory"); return -1; }
    int r = zero(d->bd, 0, ALIGN);                       /* the old table and the first volume's start */
    if (!r) r = zero(d->bd, d->sectors - ALIGN, ALIGN);  /* (an old backup GPT) */
    mbr_blank(b, d->sectors, scheme == DP_GPT);
    if (!r) r = wr(d->bd, 0, 1, b);
    if (!r && scheme == DP_GPT) {
        uint8_t g[16];
        guid(g);
        memset(b, 0, GPT_SECS * SEC);
        r = gpt_write(d->bd, d->sectors, b, g);
    }
    kfree(b);
    if (r) { fail(err, ecap, "the disk refused the write"); remount(d); return -1; }
    remount(d);
    return 0;
}

/* ── FAT32 ── */

static int mkfs_fat32(blockdev_t* bd, uint32_t base, uint32_t sectors, const char* label, char* err, int ecap) {
    uint32_t spc;
    uint64_t bytes = (uint64_t)sectors * SEC;
    if (bytes < (uint64_t)33 * 1024 * 1024) { fail(err, ecap, "FAT32 needs 33 MB at least: use exFAT"); return -1; }
    if (bytes <= (uint64_t)260 << 20) spc = 1;
    else if (bytes <= (uint64_t)8 << 30) spc = 8;
    else if (bytes <= (uint64_t)16 << 30) spc = 16;
    else if (bytes <= (uint64_t)32 << 30) spc = 32;
    else spc = 64;
    const uint32_t rsvd = 32, nfats = 2;
    uint32_t t1 = sectors - rsvd, t2 = (256 * spc + nfats) / 2;
    uint32_t fatsz = (t1 + t2 - 1) / t2;
    uint32_t clusters = (sectors - rsvd - nfats * fatsz) / spc;
    if (clusters < 65525) { fail(err, ecap, "too small for FAT32 clusters: use exFAT"); return -1; }
    uint8_t* b = (uint8_t*)kmalloc(SEC);
    if (!b) { fail(err, ecap, "out of memory"); return -1; }
    /* the boot sector (and its copy at 6) */
    memset(b, 0, SEC);
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "BANANAOS", 8);
    wr16(b + 11, SEC);
    b[13] = (uint8_t)spc;
    wr16(b + 14, (uint16_t)rsvd);
    b[16] = (uint8_t)nfats;
    b[21] = 0xF8;
    wr16(b + 24, 63);
    wr16(b + 26, 255);
    wr32(b + 28, base);
    wr32(b + 32, sectors);
    wr32(b + 36, fatsz);
    wr32(b + 44, 2);
    wr16(b + 48, 1);
    wr16(b + 50, 6);
    b[64] = 0x80;
    b[66] = 0x29;
    wr32(b + 67, random_u32());
    char vl[11];
    memset(vl, ' ', 11);
    const char* l = label && label[0] ? label : "NO NAME";
    for (int i = 0; i < 11 && l[i]; i++) vl[i] = (l[i] >= 'a' && l[i] <= 'z') ? (char)(l[i] - 32) : l[i];
    memcpy(b + 71, vl, 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    int r = zero(bd, base, rsvd);
    if (!r) r = wr(bd, base, 1, b) || wr(bd, base + 6, 1, b);
    /* FSInfo (and its copy at 7) */
    memset(b, 0, SEC);
    wr32(b, 0x41615252u);
    wr32(b + 484, 0x61417272u);
    wr32(b + 488, clusters - 1);
    wr32(b + 492, 3);
    wr32(b + 508, 0xAA550000u);
    if (!r) r = wr(bd, base + 1, 1, b) || wr(bd, base + 7, 1, b);
    /* the FATs: clusters 0, 1 and the root's (2) taken */
    for (uint32_t f = 0; f < nfats && !r; f++) {
        uint32_t at = base + rsvd + f * fatsz;
        r = zero(bd, at, fatsz);
        memset(b, 0, SEC);
        wr32(b, 0x0FFFFFF8u);
        wr32(b + 4, 0x0FFFFFFFu);
        wr32(b + 8, 0x0FFFFFFFu);
        if (!r) r = wr(bd, at, 1, b);
    }
    /* the root folder: empty, but for the volume's name */
    uint32_t root = base + rsvd + nfats * fatsz;
    if (!r) r = zero(bd, root, spc);
    if (!r && label && label[0]) {
        memset(b, 0, SEC);
        memcpy(b, vl, 11);
        b[11] = 0x08;
        r = wr(bd, root, 1, b);
    }
    kfree(b);
    if (r) fail(err, ecap, "the disk refused the write");
    return r ? -1 : 0;
}

/* ── exFAT ── */

static uint32_t boot_sum(uint32_t s, const uint8_t* p, uint32_t n, int first) {
    for (uint32_t i = 0; i < n; i++) {
        if (first && (i == 106 || i == 107 || i == 112)) continue;
        s = ((s & 1) ? 0x80000000u : 0) + (s >> 1) + p[i];
    }
    return s;
}

static int mkfs_exfat(blockdev_t* bd, uint32_t base, uint32_t sectors, const char* label, char* err, int ecap) {
    uint64_t bytes = (uint64_t)sectors * SEC;
    if (bytes < (uint64_t)2 << 20) { fail(err, ecap, "exFAT needs 2 MB at least"); return -1; }
    uint32_t spc_shift = bytes <= ((uint64_t)256 << 20) ? 3 : bytes <= ((uint64_t)32 << 30) ? 6 : 8;   /* 4 KB, 32 KB, 128 KB clusters */
    uint32_t spc = 1u << spc_shift;
    uint32_t fat_off = 128;
    uint32_t clusters = (sectors - fat_off) / spc, fat_len = 0, heap = 0;
    for (int it = 0; it < 4; it++) {
        fat_len = ((clusters + 2) * 4 + SEC - 1) / SEC;
        heap = (fat_off + fat_len + spc - 1) / spc * spc;
        clusters = (sectors - heap) / spc;
    }
    if (clusters < 16) { fail(err, ecap, "too small"); return -1; }
    /* the up-case table, compressed: a..z as A..Z, everything else itself */
    uint16_t up[32];
    int nu = 0;
    up[nu++] = 0xFFFF; up[nu++] = 0x61;
    for (int c = 'a'; c <= 'z'; c++) up[nu++] = (uint16_t)(c - 32);
    up[nu++] = 0xFFFF; up[nu++] = (uint16_t)(0x10000 - 0x7B);
    uint32_t up_bytes = (uint32_t)nu * 2;
    uint8_t upb[64];
    for (int i = 0; i < nu; i++) wr16(upb + i * 2, up[i]);
    uint32_t up_sum = 0;
    for (uint32_t i = 0; i < up_bytes; i++) up_sum = ((up_sum & 1) ? 0x80000000u : 0) + (up_sum >> 1) + upb[i];
    uint32_t cbytes = spc * SEC;
    uint32_t bmp_bytes = (clusters + 7) / 8;
    uint32_t bmp_cl = (bmp_bytes + cbytes - 1) / cbytes, up_cl = 1, root_cl = 1;
    uint32_t c_bmp = 2, c_up = c_bmp + bmp_cl, c_root = c_up + up_cl, used = bmp_cl + up_cl + root_cl;
    uint8_t* b = (uint8_t*)kmalloc(SEC * 12);
    if (!b) { fail(err, ecap, "out of memory"); return -1; }
    /* the boot region: boot sector, 8 extended ones, OEM, reserved, checksum */
    memset(b, 0, SEC * 12);
    b[0] = 0xEB; b[1] = 0x76; b[2] = 0x90;
    memcpy(b + 3, "EXFAT   ", 8);
    wr64(b + 64, base);
    wr64(b + 72, sectors);
    wr32(b + 80, fat_off);
    wr32(b + 84, fat_len);
    wr32(b + 88, heap);
    wr32(b + 92, clusters);
    wr32(b + 96, c_root);
    wr32(b + 100, random_u32());
    wr16(b + 104, 0x0100);
    b[108] = 9;
    b[109] = (uint8_t)spc_shift;
    b[110] = 1;
    b[111] = 0x80;
    b[112] = 0;
    b[510] = 0x55; b[511] = 0xAA;
    for (int s = 1; s <= 8; s++) { b[s * SEC + 510] = 0x55; b[s * SEC + 511] = 0xAA; }
    uint32_t sum = 0;
    for (int s = 0; s < 11; s++) sum = boot_sum(sum, b + s * SEC, SEC, s == 0);
    uint8_t* cs = (uint8_t*)kmalloc(SEC);
    if (!cs) { kfree(b); fail(err, ecap, "out of memory"); return -1; }
    for (int i = 0; i < 128; i++) wr32(cs + i * 4, sum);
    int r = zero(bd, base, heap);                        /* the boot regions and the FAT */
    if (!r) r = wr(bd, base, 11, b) || wr(bd, base + 11, 1, cs) || wr(bd, base + 12, 11, b) || wr(bd, base + 23, 1, cs);
    kfree(cs);
    /* the FAT: 0, 1, then the chains of the bitmap, the up-case table and the root */
    uint8_t* s = b;
    memset(s, 0, SEC);
    uint32_t fat_sec = base + fat_off, cur = 0xFFFFFFFFu;
    for (uint32_t c = 0; c < 2 + used && !r; c++) {
        uint32_t v;
        if (c == 0) v = 0xFFFFFFF8u;
        else if (c == 1) v = 0xFFFFFFFFu;
        else if (c < c_up) v = c + 1 < c_up ? c + 1 : 0xFFFFFFFFu;
        else v = 0xFFFFFFFFu;                            /* (the up-case table and the root: one cluster each) */
        uint32_t sec = c * 4 / SEC;
        if (sec != cur) {
            if (cur != 0xFFFFFFFFu) r = wr(bd, fat_sec + cur, 1, s);
            memset(s, 0, SEC);
            cur = sec;
        }
        wr32(s + c * 4 % SEC, v);
    }
    if (!r && cur != 0xFFFFFFFFu) r = wr(bd, fat_sec + cur, 1, s);
    /* the clusters: the bitmap (the first `used` bits set), the up-case table, the root */
    uint32_t heap_lba = base + heap;
    if (!r) r = zero(bd, heap_lba, used * spc);
    if (!r) {
        for (uint32_t k = 0; k < bmp_cl * spc && !r; k++) {
            memset(s, 0, SEC);
            for (uint32_t bit = k * SEC * 8; bit < used && bit < (k + 1) * SEC * 8; bit++) s[(bit / 8) % SEC] |= (uint8_t)(1u << (bit % 8));
            if (k * SEC * 8 >= used) break;
            r = wr(bd, heap_lba + k, 1, s);
        }
    }
    if (!r) {
        memset(s, 0, SEC);
        memcpy(s, upb, up_bytes);
        r = wr(bd, heap_lba + (c_up - 2) * spc, 1, s);
    }
    if (!r) {
        memset(s, 0, SEC);
        uint8_t* e = s;
        /* the volume label */
        int ln = 0;
        if (label) for (; label[ln] && ln < 11; ln++) wr16(e + 2 + ln * 2, (uint8_t)label[ln]);
        e[0] = 0x83;
        e[1] = (uint8_t)ln;
        e += 32;
        /* the allocation bitmap */
        e[0] = 0x81;
        wr32(e + 20, c_bmp);
        wr64(e + 24, bmp_bytes);
        e += 32;
        /* the up-case table */
        e[0] = 0x82;
        wr32(e + 4, up_sum);
        wr32(e + 20, c_up);
        wr64(e + 24, up_bytes);
        r = wr(bd, heap_lba + (c_root - 2) * spc, 1, s);
    }
    kfree(b);
    if (r) fail(err, ecap, "the disk refused the write");
    return r ? -1 : 0;
}

static int mkfs(blockdev_t* bd, uint32_t base, uint32_t sectors, int fs, const char* label, char* err, int ecap) {
    if (fs == DP_FAT32) return mkfs_fat32(bd, base, sectors, label, err, ecap);
    if (fs == DP_EXFAT) return mkfs_exfat(bd, base, sectors, label, err, ecap);
    fail(err, ecap, "FAT32 or exFAT");
    return -1;
}

/* ── the partition tables ── */

static int mbr_set(dp_disk_t* d, int slot, uint8_t type, uint32_t base, uint32_t sectors) {
    uint8_t* b = (uint8_t*)kmalloc(SEC);
    if (!b) return -1;
    int r = d->bd->read(d->bd, 0, 1, b);
    if (!r) {
        uint8_t* e = b + 446 + slot * 16;
        memset(e, 0, 16);
        if (type) {
            e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;        /* CHS: past what CHS can say */
            e[4] = type;
            e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
            wr32(e + 8, base);
            wr32(e + 12, sectors);
        }
        b[510] = 0x55; b[511] = 0xAA;
        r = wr(d->bd, 0, 1, b);
    }
    kfree(b);
    return r;
}

static int gpt_load(dp_disk_t* d, uint8_t* ents, uint8_t* disk_guid) {
    uint8_t* h = (uint8_t*)kmalloc(SEC);
    if (!h) return -1;
    int r = d->bd->read(d->bd, 1, 1, h);
    if (!r && memcmp(h, "EFI PART", 8)) r = -1;
    if (!r) {
        memcpy(disk_guid, h + 56, 16);
        uint32_t lba = rd32(h + 72), n = rd32(h + 80), esz = rd32(h + 84);
        if (esz != GPT_ESZ || n > GPT_ENTS) r = -1;
        else r = d->bd->read(d->bd, lba, GPT_SECS, ents);
    }
    kfree(h);
    return r;
}

int dp_create(dp_disk_t* d, int gap, uint32_t sectors, int fs, const char* label, int* number, char* err, int ecap) {
    if (check_writable(d, err, ecap)) return -1;
    if (d->scheme == DP_NONE) {
        if (d->nparts) { fail(err, ecap, "the whole disk is one volume: delete it or make a new table first"); return -1; }
        if (dp_init(d, d->sectors > 0xFFFFFFF0u ? DP_GPT : DP_MBR, err, ecap)) return -1;
        gap = 0;
    }
    if (gap < 0 || gap >= d->ngaps) { fail(err, ecap, "there is no free space there"); return -1; }
    uint32_t base = d->gap_base[gap], room = d->gap_sectors[gap];
    if (!sectors || sectors > room) sectors = room;
    if (sectors >= ALIGN) sectors = sectors / ALIGN * ALIGN;     /* (whole MiBs) */
    release(d);
    int r, num = 0;
    if (d->scheme == DP_MBR) {
        uint8_t* b = (uint8_t*)kmalloc(SEC);
        int slot = -1;
        if (b && d->bd->read(d->bd, 0, 1, b) == 0)
            for (int i = 0; i < 4 && slot < 0; i++) if (!b[446 + i * 16 + 4]) slot = i;
        kfree(b);
        if (slot < 0) { fail(err, ecap, "the MBR has 4 partitions already"); remount(d); return -1; }
        r = mbr_set(d, slot, fs == DP_FAT32 ? 0x0C : 0x07, base, sectors);
        num = slot + 1;
    } else {
        uint8_t* ents = (uint8_t*)kmalloc(GPT_SECS * SEC);
        uint8_t dg[16];
        if (!ents || gpt_load(d, ents, dg)) { kfree(ents); fail(err, ecap, "the GPT could not be read"); remount(d); return -1; }
        int slot = -1;
        for (uint32_t i = 0; i < GPT_ENTS && slot < 0; i++) {
            int empty = 1;
            for (int k = 0; k < 16; k++) if (ents[i * GPT_ESZ + k]) empty = 0;
            if (empty) slot = (int)i;
        }
        if (slot < 0) { kfree(ents); fail(err, ecap, "the GPT is full"); remount(d); return -1; }
        uint8_t* e = ents + slot * GPT_ESZ;
        memset(e, 0, GPT_ESZ);
        memcpy(e, GUID_BASIC_DATA, 16);
        guid(e + 16);
        wr64(e + 32, base);
        wr64(e + 40, (uint64_t)base + sectors - 1);
        const char* nm = label && label[0] ? label : "Basic data partition";
        for (int i = 0; nm[i] && i < 36; i++) wr16(e + 56 + i * 2, (uint8_t)nm[i]);
        r = gpt_write(d->bd, d->sectors, ents, dg);
        kfree(ents);
        num = slot + 1;
    }
    if (!r) r = mkfs(d->bd, base, sectors, fs, label, err, ecap);
    else fail(err, ecap, "the disk refused the write");
    if (number) *number = num;
    remount(d);
    return r ? -1 : 0;
}

int dp_delete(dp_disk_t* d, int number, char* err, int ecap) {
    if (check_writable(d, err, ecap)) return -1;
    dp_part_t* p = dp_part(d, number);
    if (!p) { fail(err, ecap, "no such partition"); return -1; }
    release(d);
    int r;
    if (number == 0) r = zero(d->bd, 0, 64);                                   /* a whole-disk volume */
    else if (d->scheme == DP_MBR) {
        if (number > 4) { fail(err, ecap, "logical partitions cannot be deleted here yet"); remount(d); return -1; }
        r = mbr_set(d, number - 1, 0, 0, 0);
    } else {
        uint8_t* ents = (uint8_t*)kmalloc(GPT_SECS * SEC);
        uint8_t dg[16];
        if (!ents || gpt_load(d, ents, dg)) { kfree(ents); fail(err, ecap, "the GPT could not be read"); remount(d); return -1; }
        memset(ents + (number - 1) * GPT_ESZ, 0, GPT_ESZ);
        r = gpt_write(d->bd, d->sectors, ents, dg);
        kfree(ents);
    }
    if (!r) zero(d->bd, p->base, p->sectors < 64 ? p->sectors : 64);          /* (so it is not found again) */
    if (r) fail(err, ecap, "the disk refused the write");
    remount(d);
    return r ? -1 : 0;
}

int dp_format(dp_disk_t* d, int number, int fs, const char* label, char* err, int ecap) {
    if (check_writable(d, err, ecap)) return -1;
    uint32_t base = 0, sectors = d->sectors;
    if (number > 0) {
        dp_part_t* p = dp_part(d, number);
        if (!p) { fail(err, ecap, "no such partition"); return -1; }
        base = p->base;
        sectors = p->sectors;
    } else if (d->scheme != DP_NONE && d->nparts) { fail(err, ecap, "the disk has partitions: format one of them"); return -1; }
    release(d);
    int r = mkfs(d->bd, base, sectors, fs, label, err, ecap);
    /* the table's type byte follows the filesystem */
    if (!r && number > 0 && number <= 4 && d->scheme == DP_MBR) {
        uint8_t* b = (uint8_t*)kmalloc(SEC);
        if (b && d->bd->read(d->bd, 0, 1, b) == 0) {
            b[446 + (number - 1) * 16 + 4] = fs == DP_FAT32 ? 0x0C : 0x07;
            wr(d->bd, 0, 1, b);
        }
        kfree(b);
    }
    remount(d);
    return r;
}

int dp_mount(dp_disk_t* d, int number, char* err, int ecap) {
    if (d->system || d->cdrom || !d->bd) { fail(err, ecap, "not a drive Disk Management mounts"); return -1; }
    dp_part_t* p = dp_part(d, number);
    if (!p) { fail(err, ecap, "no such partition"); return -1; }
    if (p->mnt) return 0;
    char e[96];
    int mnt = volume_mount_at(d->bd, p->base, NULL, e, sizeof(e));
    fill(d, d->bd);
    if (mnt <= 0) { fail(err, ecap, e); return -1; }
    return 0;
}

int dp_unmount(dp_disk_t* d, int number, char* err, int ecap) {
    dp_part_t* p = dp_part(d, number);
    if (!p || !p->mnt) { fail(err, ecap, "it is not mounted"); return -1; }
    blockdev_eject(p->mnt);
    fill(d, d->bd);
    return 0;
}
