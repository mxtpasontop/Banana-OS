/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diskcmd.h"
#include "../kernel/diskpart.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/kheap.h"
#include "../kernel/gui.h"

/*
 * diskmgr                         the disks: size, table, free space
 * diskmgr list                    ... and every volume on them
 * diskmgr show DISK               one disk's partitions and free space
 * diskmgr init DISK mbr|gpt -y    a new, empty partition table
 * diskmgr create DISK [SIZE] [fat32|exfat] [LABEL] -y
 *                                 a volume in the biggest free space (SIZE: 500M, 2G, all)
 * diskmgr format DISK[pN] fat32|exfat [LABEL] -y
 * diskmgr delete DISKpN -y
 * diskmgr mount DISKpN | unmount DISKpN
 * diskmgr gui                     the Disk Management window (diskmgmt.msc too)
 * mkfs.fat / mkfs.vfat / mkfs.exfat [-n LABEL] DISK[pN]
 */

static void say(const char* s) { terminal_writeln(s); }
static void head(const char* s) { terminal_write_color(s, VGA_COLOR_YELLOW, VGA_COLOR_BLACK); terminal_writeln(""); }

static const char* scheme_name(int s) { return s == DP_GPT ? "GPT" : s == DP_MBR ? "MBR" : "none"; }

static uint64_t free_of(const dp_disk_t* d) {
    uint64_t f = 0;
    for (int g = 0; g < d->ngaps; g++) f += (uint64_t)d->gap_sectors[g] * 512u;
    return f;
}

static void disk_line(const dp_disk_t* d) {
    char sz[16], fr[16], b[160];
    dp_size_text((uint64_t)d->sectors * 512u, sz, sizeof(sz));
    dp_size_text(free_of(d), fr, sizeof(fr));
    ksnprintf(b, sizeof(b), "%-7s %-9s %8s  %-4s  %8s free  %s%s", d->name, d->kind, d->cdrom ? "-" : sz, d->cdrom ? "" : scheme_name(d->scheme),
              d->cdrom ? "-" : fr, d->model, d->system ? "  (Banana OS)" : "");
    say(b);
}

static void part_line(const dp_disk_t* d, const dp_part_t* p) {
    char sz[16], b[200], use[40] = "";
    dp_size_text((uint64_t)p->sectors * 512u, sz, sizeof(sz));
    if (p->total) {
        char t[16];
        dp_size_text(p->free_bytes, t, sizeof(t));
        ksnprintf(use, sizeof(use), "%s free", t);
    }
    char name[24];
    if (p->number) ksnprintf(name, sizeof(name), "%sp%d", d->name, p->number);
    else ksnprintf(name, sizeof(name), "%s", d->name);
    ksnprintf(b, sizeof(b), "  %-9s %8s  %-6s %-12s %-14s %-12s %s", name, sz, p->fs[0] ? p->fs : "?", p->label[0] ? p->label : "-",
              p->where[0] ? p->where : "(not mounted)", use, p->type);
    say(b);
}

static void show(const dp_disk_t* d) {
    disk_line(d);
    if (d->cdrom) return;
    for (int i = 0; i < d->nparts; i++) part_line(d, &d->parts[i]);
    for (int g = 0; g < d->ngaps; g++) {
        char sz[16], b[96];
        dp_size_text((uint64_t)d->gap_sectors[g] * 512u, sz, sizeof(sz));
        ksnprintf(b, sizeof(b), "  free%-5d %8s  unallocated (sectors %u-%u)", g + 1, sz, d->gap_base[g], d->gap_base[g] + d->gap_sectors[g] - 1);
        say(b);
    }
    if (!d->nparts && !d->ngaps) say(d->scheme == DP_NONE ? "  (no partition table: diskmgr init)" : "  (empty)");
}

static int parse_fs(const char* s) {
    if (!strcasecmp(s, "fat32") || !strcasecmp(s, "fat") || !strcasecmp(s, "vfat")) return DP_FAT32;
    if (!strcasecmp(s, "exfat")) return DP_EXFAT;
    return 0;
}

/* "500M", "2G", "100" (MB), "all" -> sectors (0 = all); -1 if not a size */
static int64_t parse_size(const char* s) {
    if (!strcasecmp(s, "all") || !strcasecmp(s, "max")) return 0;
    uint32_t v;
    int k = k_parse_u32(s, &v);
    if (k <= 0) return -1;
    char u = s[k];
    if (u && s[k + 1] && strcasecmp(s + k + 1, "B") && strcasecmp(s + k + 1, "iB")) return -1;
    uint64_t mb = v;
    if (u == 'G' || u == 'g') mb = (uint64_t)v * 1024;
    else if (u == 'T' || u == 't') mb = (uint64_t)v * 1024 * 1024;
    else if (u && u != 'M' && u != 'm') return -1;
    return (int64_t)(mb * 2048);
}

static int has_yes(int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes")) return 1;
    return 0;
}

static void report(int r, const char* err, const char* ok, dp_disk_t* d) {
    if (r) { terminal_write_color("diskmgr: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK); terminal_writeln(err); return; }
    say(ok);
    show(d);
}

static int mkfs_cmd(int argc, char** argv, int fs, dp_disk_t* disks, int n) {
    const char* label = "";
    const char* dev = NULL;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "-L")) && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "-F") && i + 1 < argc) i++;
        else if (argv[i][0] != '-') dev = argv[i];
    }
    if (!dev) { say(fs == DP_FAT32 ? "usage: mkfs.fat [-n LABEL] DISK[pN]   (lsblk / diskmgr list the disks)" : "usage: mkfs.exfat [-n LABEL] DISK[pN]"); return 1; }
    if (!strncmp(dev, "/dev/", 5)) dev += 5;
    int num;
    dp_disk_t* d = dp_find(disks, n, dev, &num);
    if (!d) { say("mkfs: no such disk or partition (diskmgr list)"); return 1; }
    char err[96];
    int r = dp_format(d, num < 0 ? 0 : num, fs, label, err, sizeof(err));
    report(r, err, fs == DP_FAT32 ? "formatted as FAT32" : "formatted as exFAT", d);
    return 1;
}

int diskcmd_dispatch(int argc, char** argv) {
    if (argc < 1) return 0;
    int mk = !strcmp(argv[0], "mkfs.fat") || !strcmp(argv[0], "mkfs.vfat") ? DP_FAT32 : !strcmp(argv[0], "mkfs.exfat") ? DP_EXFAT : 0;
    if (!strcmp(argv[0], "diskmgmt.msc") || !strcmp(argv[0], "diskmgmt")) {
        if (!gui_open_diskmgr()) say("diskmgr: the Disk Management window is part of the desktop (startx); here: diskmgr");
        return 1;
    }
    if (!mk && strcmp(argv[0], "diskmgr") && strcmp(argv[0], "diskpart")) return 0;
    const char* sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "gui")) {
        if (!gui_open_diskmgr()) say("diskmgr: the Disk Management window is part of the desktop (startx)");
        return 1;
    }
    dp_disk_t* disks = (dp_disk_t*)kmalloc(sizeof(dp_disk_t) * DP_MAX_DISKS);
    if (!disks) { say("diskmgr: out of memory"); return 1; }
    int n = dp_scan(disks, DP_MAX_DISKS);
    char err[96];
    if (mk) { mkfs_cmd(argc, argv, mk, disks, n); kfree(disks); return 1; }
    int num = -1;
    dp_disk_t* d = argc > 2 ? dp_find(disks, n, argv[2], &num) : NULL;
    if (!sub[0]) {
        head("DISK    KIND          SIZE  TABLE      FREE        MODEL");
        for (int i = 0; i < n; i++) disk_line(&disks[i]);
    } else if (!strcmp(sub, "list")) {
        for (int i = 0; i < n; i++) show(&disks[i]);
    } else if (argc < 3) {
        say("usage: diskmgr [list | show DISK | init DISK mbr|gpt | create DISK [SIZE] [fat32|exfat] [LABEL] |");
        say("        format DISK[pN] fat32|exfat [LABEL] | delete DISKpN | mount DISKpN | unmount DISKpN | gui]");
        say("       (the changes ask for -y: they erase what is there)");
    } else if (!d) {
        say("diskmgr: no such disk (diskmgr lists them: sata1, usb0, usb0p1...)");
    } else if (!strcmp(sub, "show")) {
        show(d);
    } else if (!strcmp(sub, "mount") || !strcmp(sub, "unmount") || !strcmp(sub, "umount")) {
        if (num < 0) num = d->nparts == 1 ? d->parts[0].number : -1;
        if (num < 0) say("diskmgr: which partition? (usb0p1)");
        else {
            int r = sub[0] == 'm' ? dp_mount(d, num, err, sizeof(err)) : dp_unmount(d, num, err, sizeof(err));
            report(r, err, sub[0] == 'm' ? "mounted" : "unmounted - safe to remove", d);
        }
    } else if (!has_yes(argc, argv)) {
        char b[120];
        ksnprintf(b, sizeof(b), "diskmgr: this erases what is on %s - add -y to go ahead", argv[2]);
        say(b);
    } else if (!strcmp(sub, "init")) {
        int sc = argc > 3 && !strcasecmp(argv[3], "gpt") ? DP_GPT : argc > 3 && !strcasecmp(argv[3], "mbr") ? DP_MBR : 0;
        if (!sc) say("usage: diskmgr init DISK mbr|gpt -y");
        else report(dp_init(d, sc, err, sizeof(err)), err, "new partition table written", d);
    } else if (!strcmp(sub, "create")) {
        int64_t size = 0;
        int fs = DP_FAT32, gap = -1;
        const char* label = "";
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes")) continue;
            if (parse_fs(argv[i])) fs = parse_fs(argv[i]);
            else if (!strncmp(argv[i], "free", 4) && argv[i][4]) { uint32_t g; if (k_parse_u32(argv[i] + 4, &g) > 0) gap = (int)g - 1; }
            else if (parse_size(argv[i]) >= 0 && !label[0]) size = parse_size(argv[i]);
            else label = argv[i];
        }
        if (gap < 0) {                                   /* the biggest free space */
            uint32_t best = 0;
            for (int g = 0; g < d->ngaps; g++) if (d->gap_sectors[g] > best) { best = d->gap_sectors[g]; gap = g; }
            if (gap < 0) gap = 0;
        }
        /* FAT32 needs 33 MB: smaller volumes get exFAT unless asked */
        int pn = 0;
        int r = dp_create(d, gap, (uint32_t)size, fs, label, &pn, err, sizeof(err));
        char ok[64];
        ksnprintf(ok, sizeof(ok), "created %sp%d", d->name, pn);
        report(r, err, ok, d);
    } else if (!strcmp(sub, "format")) {
        int fs = argc > 3 ? parse_fs(argv[3]) : 0;
        const char* label = argc > 4 && strcmp(argv[4], "-y") ? argv[4] : "";
        if (!fs) say("usage: diskmgr format DISK[pN] fat32|exfat [LABEL] -y");
        else report(dp_format(d, num < 0 ? 0 : num, fs, label, err, sizeof(err)), err, "formatted", d);
    } else if (!strcmp(sub, "delete")) {
        if (num < 0) num = d->nparts == 1 && d->scheme == DP_NONE ? 0 : -1;
        if (num < 0) say("diskmgr: which partition? (diskmgr delete usb0p1 -y)");
        else report(dp_delete(d, num, err, sizeof(err)), err, "deleted", d);
    } else say("diskmgr: unknown: try diskmgr with no words");
    kfree(disks);
    return 1;
}
