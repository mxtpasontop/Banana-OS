/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "disk.h"
#include "timer.h"
#include "atapi.h"
#include "ahci.h"
#include "nvme.h"
#include "virtio_blk.h"
#include "virtio_scsi.h"
#include "sdhci.h"
#include "kstring.h"
#include "blockdev.h"
#include "iowait.h"
#include "serial.h"

int disk_probe(ata_disk_t* out, int max) {
    ata_disk_t ide[4];
    ata_lock(ATA_BUS_PRIMARY);
    ata_lock(ATA_BUS_SECONDARY);
    ata_probe_disks(ide);
    ata_unlock(ATA_BUS_SECONDARY);
    ata_unlock(ATA_BUS_PRIMARY);
    int n = 0;
    for (int i = 0; i < 4 && n < max; i++)
        if (ide[i].present) out[n++] = ide[i];
    n += ahci_probe(out + n, max - n);
    n += nvme_probe(out + n, max - n);
    n += virtio_blk_probe(out + n, max - n);
    n += virtio_scsi_probe(out + n, max - n);
    n += sdhci_probe(out + n, max - n);
    return n;
}

/* ── the controllers' DMA and the caller's buffer ────────────────────
 * SATA, NVMe and IDE's bus-master DMA move data at physical addresses.
 * The kernel's memory is identity-mapped (the address is the physical
 * one), but an app's mmap region (kernel/uvm.h) is not, and IDE's DMA (and
 * an AHCI controller without 64-bit addressing) reaches only the first
 * 4 GiB: such a buffer goes through one of the kernel's, a piece at a time. */

#define UVM_HOLE  0x0000600000000000ull       /* (UVM_BASE and up: an app's mmap region) */
#define BOUNCE    (256u * 1024u)
static uint8_t      g_bounce[BOUNCE] __attribute__((aligned(4096)));
static iowait_t g_bounce_lock = IOWAIT_INIT;

static int below_4g(const void* p, uint32_t bytes) { return (uint64_t)(uintptr_t)p + bytes <= 0x100000000ull; }

static int needs_bounce(const ata_disk_t* d, const void* p, uint32_t bytes) {
    if (d->vblk || d->scsi) return 0;                   /* (virtio copies through its own) */
    if (d->mmc) return 0;                               /* (the processor copies: any address) */
#ifdef __x86_64__
    if ((uint64_t)(uintptr_t)p >= UVM_HOLE) return 1;
#endif
    if (d->nvme) return 0;
    if (d->ahci_port >= 0) return !ahci_addr64(d->ahci_port) && !below_4g(p, bytes);
    return ata_dma_available() && !below_4g(p, bytes);  /* (PIO: the processor copies - any address) */
}

/* IDE: by DMA where it can, by PIO where it cannot; the channel held */
static int ide_rw(const ata_disk_t* d, uint32_t lba, uint32_t count, uint8_t* b, int write) {
    int rc = 0;
    ata_lock(d->bus);
    while (count && rc == 0) {
        uint32_t max = ata_dma_max(d->bus, d->is_slave), n = count > 1024u ? 1024u : count;   /* (512 KiB a command) */
        if (n > max) n = max;
        int r = ata_dma_rw(d->bus, d->is_slave, lba, n, b, write);
        if (r == -1) {
            klog("ide: a DMA %s failed at %u - PIO from now on\n", write ? "write" : "read", lba);
            ata_dma_disable();
        }
        if (r != 0) {
            if (n > 255) n = 255;
            r = write ? ata_write_sectors(d->bus, d->is_slave, lba, (uint8_t)n, b)
                      : ata_read_sectors(d->bus, d->is_slave, lba, (uint8_t)n, b);
        }
        rc = r;
        lba += n;
        count -= n;
        b += n * 512u;
    }
    ata_unlock(d->bus);
    return rc;
}

static int raw_rw(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf, int write) {
    if (d->nvme) return write ? nvme_write(d->nvme - 1, lba, count, buf) : nvme_read(d->nvme - 1, lba, count, buf);
    if (d->vblk) return write ? virtio_blk_write(d->vblk - 1, lba, count, buf) : virtio_blk_read(d->vblk - 1, lba, count, buf);
    if (d->scsi) return write ? virtio_scsi_write(d->scsi - 1, lba, count, buf) : virtio_scsi_read(d->scsi - 1, lba, count, buf);
    if (d->mmc) return write ? sdhci_write(d->mmc - 1, lba, count, buf) : sdhci_read(d->mmc - 1, lba, count, buf);
    if (d->ahci_port >= 0) return write ? ahci_write(d->ahci_port, lba, count, buf) : ahci_read(d->ahci_port, lba, count, buf);
    return ide_rw(d, lba, count, (uint8_t*)buf, write);
}

static int disk_rw(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf, int write) {
    if (!needs_bounce(d, buf, count * 512u)) return raw_rw(d, lba, count, buf, write);
    uint8_t* b = (uint8_t*)buf;
    int rc = 0;
    while (count && rc == 0) {
        uint32_t n = count > BOUNCE / 512u ? BOUNCE / 512u : count;
        io_lock(&g_bounce_lock);
        if (write) memcpy(g_bounce, b, n * 512u);
        rc = raw_rw(d, lba, n, g_bounce, write);
        if (!write && rc == 0) memcpy(b, g_bounce, n * 512u);
        io_unlock(&g_bounce_lock);
        lba += n;
        count -= n;
        b += n * 512u;
    }
    return rc;
}

/* what the disks did since boot (Task Manager): bytes read / written and
 * the time spent waiting for them (plain counters: a lost update now and
 * then only blurs a statistic) */
static uint64_t g_io_rd, g_io_wr, g_io_busy_ns;

static int counted_rw(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf, int write) {
    uint64_t t0 = timer_counter();
    int rc = disk_rw(d, lba, count, buf, write);
    g_io_busy_ns += timer_counter() - t0;
    if (write) g_io_wr += (uint64_t)count * 512u;
    else g_io_rd += (uint64_t)count * 512u;
    return rc;
}

void disk_io_stats(uint64_t* rd, uint64_t* wr, uint64_t* busy_ns) {
    if (rd) *rd = g_io_rd;
    if (wr) *wr = g_io_wr;
    if (busy_ns) *busy_ns = g_io_busy_ns;
}

int disk_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf) { return counted_rw(d, lba, count, buf, 0); }
int disk_write(const ata_disk_t* d, uint32_t lba, uint32_t count, const void* buf) { return counted_rw(d, lba, count, (void*)buf, 1); }

int disk_flush(const ata_disk_t* d) {
    if (d->nvme) return nvme_flush(d->nvme - 1);
    if (d->vblk) return virtio_blk_flush(d->vblk - 1);
    if (d->scsi) return virtio_scsi_flush(d->scsi - 1);
    if (d->mmc) return 0;                               /* (no write cache turned on) */
    if (d->ahci_port >= 0) return ahci_flush(d->ahci_port);
    if (d->is_atapi) return 0;
    ata_lock(d->bus);
    int rc = ata_flush(d->bus, d->is_slave);
    ata_unlock(d->bus);
    return rc;
}

static int cd_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf);

/* (counted like the disks: Task Manager) */
int disk_cd_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf) {
    uint64_t t0 = timer_counter();
    int rc = cd_read(d, lba, count, buf);
    g_io_busy_ns += timer_counter() - t0;
    g_io_rd += (uint64_t)count * 2048u;
    return rc;
}

static int cd_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf) {
    if (d->ahci_port >= 0) {
        if (!needs_bounce(d, buf, count * 2048u)) return ahci_atapi_read(d->ahci_port, lba, count, buf);
        uint8_t* b = (uint8_t*)buf;
        int rc = 0;
        while (count && rc == 0) {
            uint32_t n = count > BOUNCE / 2048u ? BOUNCE / 2048u : count;
            io_lock(&g_bounce_lock);
            rc = ahci_atapi_read(d->ahci_port, lba, n, g_bounce);
            if (rc == 0) memcpy(b, g_bounce, n * 2048u);
            io_unlock(&g_bounce_lock);
            lba += n;
            count -= n;
            b += n * 2048u;
        }
        return rc;
    }
    ata_lock(d->bus);
    int rc = atapi_read_blocks(d->bus, d->is_slave, lba, count, buf);
    ata_unlock(d->bus);
    return rc;
}

int disk_cd_iso_size(const ata_disk_t* d, uint32_t* out_bytes) {
    if (d->ahci_port < 0) {
        ata_lock(d->bus);
        int rc = atapi_iso_size(d->bus, d->is_slave, out_bytes);
        ata_unlock(d->bus);
        return rc;
    }
    static uint8_t pvd[2048] __attribute__((aligned(16)));
    if (ahci_atapi_read(d->ahci_port, 16, 1, pvd) != 0) return -1;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return -1;
    uint32_t blocks = (uint32_t)pvd[80] | (uint32_t)pvd[81] << 8 | (uint32_t)pvd[82] << 16 | (uint32_t)pvd[83] << 24;
    *out_bytes = blocks * 2048u;
    return 0;
}

void disk_describe(const ata_disk_t* d, char* out, int cap) {
    if (d->nvme) ksnprintf(out, (size_t)cap, "NVMe #%d", d->nvme - 1);
    else if (d->vblk) ksnprintf(out, (size_t)cap, "virtio #%d", d->vblk - 1);
    else if (d->scsi) ksnprintf(out, (size_t)cap, "SCSI #%d", d->scsi - 1);
    else if (d->mmc) ksnprintf(out, (size_t)cap, "%s #%d", sdhci_is_emmc(d->mmc - 1) ? "eMMC" : "SD card", d->mmc - 1);
    else if (d->ahci_port >= 0) ksnprintf(out, (size_t)cap, "SATA #%d", d->ahci_port);
    else ksnprintf(out, (size_t)cap, "IDE %s %s", d->bus == ATA_BUS_PRIMARY ? "primary" : "secondary",
                   d->is_slave ? "slave" : "master");
}

/* ── the extra IDE / SATA disks as block devices ─────────────────────
 * NVMe and virtio disks register themselves (kernel/nvme.c, virtio_blk.c);
 * the IDE and SATA hard disks did not, so a second disk's partitions were
 * never mounted. Every one but a disk holding Banana OS becomes a block
 * device here, its volumes mounted by the automount task (/mnt/sata, /mnt/ide). */

static ata_disk_t g_extra[DISK_MAX];
static int        g_nextra;

static int extra_read(blockdev_t* bd, uint32_t lba, uint32_t count, void* buf) {
    return disk_read((const ata_disk_t*)bd->priv, lba, count, buf);
}

static int extra_write(blockdev_t* bd, uint32_t lba, uint32_t count, const void* buf) {
    const ata_disk_t* d = (const ata_disk_t*)bd->priv;
    if (disk_write(d, lba, count, buf) != 0) return -1;
    return disk_flush(d);
}

int disk_same(const ata_disk_t* a, const ata_disk_t* b) {
    if (a->nvme || b->nvme) return a->nvme == b->nvme;
    if (a->vblk || b->vblk) return a->vblk == b->vblk;
    if (a->scsi || b->scsi) return a->scsi == b->scsi;
    if (a->mmc || b->mmc) return a->mmc == b->mmc;
    if (a->ahci_port >= 0 || b->ahci_port >= 0) return a->ahci_port == b->ahci_port;
    return a->bus == b->bus && a->is_slave == b->is_slave;
}

blockdev_t* disk_blockdev(const ata_disk_t* d) {
    char name[16];
    if (d->scsi) return virtio_scsi_blockdev(d->scsi - 1);
    if (d->mmc) return sdhci_blockdev(d->mmc - 1);
    if (d->nvme) ksnprintf(name, sizeof(name), "nvme%d", d->nvme - 1);
    else if (d->vblk) ksnprintf(name, sizeof(name), "virtio%d", d->vblk - 1);
    else {
        for (int i = 0; i < BLOCKDEV_MAX; i++) {
            blockdev_t* bd = blockdev_get(i);
            if (bd && (bd->read == extra_read) && disk_same((const ata_disk_t*)bd->priv, d)) return bd;
        }
        return NULL;
    }
    return blockdev_find(name);
}

void disk_attach_extra(int (*holds_system)(const ata_disk_t* d)) {
    ata_disk_t all[DISK_MAX];
    int n = disk_probe(all, DISK_MAX);
    for (int i = 0; i < n && g_nextra < DISK_MAX; i++) {
        const ata_disk_t* d = &all[i];
        if (d->is_atapi || !d->sectors) continue;
        if (holds_system && holds_system(d)) {             /* Banana OS's own disk: never mounted */
            blockdev_set_system(disk_blockdev(d));
            continue;
        }
        if (d->nvme || d->vblk || d->scsi || d->mmc || disk_blockdev(d)) continue;   /* (already block devices) */
        g_extra[g_nextra] = *d;
        const char* kind = d->ahci_port >= 0 ? "sata" : "ide";
        blockdev_t* bd = blockdev_register_kind(kind, d->model[0] ? d->model : "hard disk", d->sectors, 512,
                                                extra_read, extra_write, &g_extra[g_nextra]);
        if (bd) g_nextra++;
    }
    blockdev_start_mounting();
}
