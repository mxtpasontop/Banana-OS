/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "selftest.h"
#include "shell.h"
#include "../kernel/gpu.h"
#include "../kernel/gpu_virgl.h"
#include "../kernel/disk.h"
#include "../kernel/iowait.h"
#include "../kernel/idt.h"
#include "../kernel/fpu.h"
#include "../kernel/bootmenu_test.h"
#include "../kernel/bootmem_test.h"
#include "../kernel/acpi_sleep.h"
#include "../kernel/sysinfo.h"
#include "../kernel/kheap.h"
#include "../kernel/tlb.h"
#include "../kernel/boottime.h"
#include "../kernel/font.h"
#include "../kernel/utf8.h"
#include "../kernel/pci.h"
#include "../kernel/uspace.h"
#include "../kernel/mouse_capture.h"
#include "../kernel/usb.h"
#include "../kernel/gui.h"
#include "../kernel/termcolors.h"
#include "../kernel/settings.h"
#include "../kernel/gfx.h"
#include "../kernel/browser_test.h"
#include "ipv6test.h"
#include "../net/ipv6.h"
#include "../net/tcp.h"
#include "../kernel/fs.h"
#include "../kernel/kstring.h"
#include "../kernel/terminal.h"
#include "../kernel/timer.h"
#include "../kernel/timer_math.h"
#include "../kernel/task.h"
#include "../kernel/app.h"
#include "../kernel/pkg.h"
#include "../kernel/hmem.h"
#include "../kernel/smp.h"
#include "../kernel/zip.h"
#include "../kernel/clipboard.h"
#include "../kernel/builtin_apps.h"
#include "../kernel/zswap.h"
#include "../kernel/deflate.h"
#include "../kernel/tz.h"
#include "../kernel/keyboard.h"
#include "../kernel/rtc.h"
#include "datecmd.h"

/*
 * selftest [virgl | gpu | ring3 | memory | limits | cores] - checks the system features
 * from inside Banana OS, one ok / FAIL line each, and a summary. What must
 * run as an app is done by systest (sdk/examples/systest), installed from
 * the copy built into the kernel: the exit code of each of its modes is
 * the result. (Filesystems: fstest.)
 */

/* a built-in example package (kernel/builtin_apps.c: in RAM on the live CD,
 * a file on the disk once installed) written to path: 0, or -1 */
static int write_ex(const char* path, const char* file) {
    char* d;
    uint32_t n;
    if (builtin_package(file, &d, &n) != 0) return -1;
    int rc = fs_write_path(path, d, n) < 0 ? -1 : 0;
    kfree(d);
    return rc;
}

static int g_fail, g_ok;

static void report(const char* what, int ok, const char* note) {
    terminal_write_color(ok ? "  ok    " : "  FAIL  ", ok ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    char line[160];
    ksnprintf(line, sizeof(line), "%s%s%s", what, note && note[0] ? " - " : "", note ? note : "");
    terminal_writeln(line);
    if (ok) g_ok++; else g_fail++;
}

static void skip(const char* what, const char* why) {
    terminal_write_color("  skip  ", VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    char line[160];
    ksnprintf(line, sizeof(line), "%s - %s", what, why);
    terminal_writeln(line);
}

static void section(const char* name) {
    terminal_write_color(name, VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

/* systest from the kernel's own copy (once a boot): 0, or -1 */
static int install(void) {
    static int done;
    if (done) return 0;
    const char* tmp = "/tmp/systest.bpk";
    char msg[160];
    if (write_ex(tmp, "systest.bpk") < 0 || pkg_install(tmp, msg, sizeof(msg)) != 0) {
        report("install systest", 0, msg);
        return -1;
    }
    fs_delete(tmp, 0);
    done = 1;
    return 0;
}

/* runs `systest a b c` in this shell: its exit code (-1 if it did not start) */
static int run(const char* a, const char* b, const char* c) {
    char* argv[4] = { "systest", (char*)a, (char*)b, (char*)c };
    int argc = c ? 4 : b ? 3 : 2;
    char err[96];
    int rc = pkg_run("systest", argc, argv, 0, err, sizeof(err));
    if (rc < 0 && err[0]) { terminal_write("  (systest: "); terminal_write(err); terminal_writeln(")"); }
    return rc;
}

static void t_ring3(void) {
    section("apps in ring 3 (build 141)");
#ifdef __x86_64__
    report("an app runs in user mode", run("ring", NULL, NULL) == 0, "");
#else
    skip("an app runs in user mode", "the 32-bit kernel keeps apps in ring 0");
#endif
}

static void t_memory(void) {
    section("memory above 4 GiB (build 142)");
    uint64_t total = hmem_total_bytes();
    char note[96];
    if (total < (64ull << 20)) {
        skip("RAM above 4 GiB", "none here (QEMU: MEM=8G tools/dev.sh run)");
        report("an app gets 64 MiB and every byte holds", run("mem", "64", NULL) == 0, "");
        return;
    }
    ksnprintf(note, sizeof(note), "%u MiB found", (uint32_t)(total >> 20));
    report("RAM above 4 GiB", 1, note);
    uint64_t before = hmem_used_bytes();
    report("an app's 64 MiB come from above 4 GiB", run("mem", "64", "high") == 0, "");
    ksnprintf(note, sizeof(note), "%u MiB in use before, %u after", (uint32_t)(before >> 20), (uint32_t)(hmem_used_bytes() >> 20));
    report("it is given back when the app ends", hmem_used_bytes() <= before, note);
}

static void t_limits(void) {
    section("higher limits (build 143)");
    report("an app with 60 threads at once", run("threads", "60", NULL) == 0, "");
    report("an app with 60 files open at once", run("files", "60", NULL) == 0, "");
    /* 24 apps at once (8 used to be the most) */
    char prog[FS_PATH_LEN], err[96];
    ksnprintf(prog, sizeof(prog), "%s/systest/app-%s", PKG_DIR, BANANA_ARCH);
    char* argv[3] = { "systest", "idle", "3000" };
    int base = app_count(), started = 0;
    for (int i = 0; i < 24; i++) if (app_spawn(prog, 3, argv, err, sizeof(err)) == 0) started++;
    int peak = app_count() - base;
    uint32_t t0 = timer_ms();
    while (app_count() > base && timer_ms() - t0 < 30000) task_sleep_ms(100);
    char note[96];
    ksnprintf(note, sizeof(note), "%d started, %d running together, %s", started, peak,
              app_count() > base ? "some never ended" : "all ended");
    report("24 apps at once", started == 24 && peak >= 24 && app_count() == base, note);
}

static void t_cores(void) {
    section("system calls on every core (build 144)");
    int n = cpu_count();
    int rc = run("cores", NULL, NULL);
    char note[96];
    if (rc < 100) { report("threads calling malloc / free / a lock", 0, "wrong totals or a crash"); return; }
    int cores = rc - 100;
    ksnprintf(note, sizeof(note), "on %d of %d core%s", cores, n, n == 1 ? "" : "s");
    report("threads calling malloc / free / a lock", 1, note);
    if (n < 2) skip("they spread over the cores", "one core here (QEMU: SMP=4)");
    else report("they spread over the cores", cores >= 2, cores >= 2 ? "" : "all stayed on one core");
}

static void t_clock(void) {
    section("shared monotonic clock (build 199)");
    timer_math_tests(report);
    char note[96];
    report("clock source selected",1,timer_clock_source());
    rtc_datetime_t first,last;
    int hw=rtc_read_hardware(&first)==0;
    uint64_t start=timer_counter();
    task_sleep_ms(1200);
    uint64_t elapsed=(timer_counter()-start)/1000000ull;
    ksnprintf(note,sizeof(note),"%llu ms for a 1200 ms sleep",elapsed);
    /* task_sleep_ms uses millisecond deadlines: a fractional first tick
     * can make its measured duration just under 1200 whole milliseconds. */
    report("sleep respects its millisecond deadline",elapsed>=1199 && elapsed<5000,note);
    if(hw && rtc_read_hardware(&last)==0) {
        unsigned a=first.hour*3600u+first.minute*60u+first.second;
        unsigned b=last.hour*3600u+last.minute*60u+last.second;
        unsigned seconds=(b+86400u-a)%86400u;
        ksnprintf(note,sizeof(note),"CMOS advanced %u s; monotonic clock %llu ms",seconds,elapsed);
        report("elapsed time agrees with the independent CMOS clock",seconds>=1 && seconds<=5 && elapsed+1000>=seconds*1000ull && elapsed<=(seconds+1)*1000ull,note);
    } else skip("elapsed time agrees with CMOS","no readable hardware RTC");
    int rc=run("clock",NULL,NULL);
    report("user clock reads stay monotonic across core migrations",rc>=100,"");
    if(cpu_count()>1) {
        ksnprintf(note,sizeof(note),"read on %d of %d cores",rc>=100?rc-100:0,cpu_count());
        report("clock reads exercise multiple cores",rc>=102,note);
    }
}

static void t_malloc(void) {
    section("malloc in the app (build 190)");
    int rc = run("malloc", NULL, NULL);
    if (rc < 100) { report("random blocks, realloc and alignment", 0, "a block changed, out of memory or a crash"); return; }
    report("random blocks, realloc and alignment", 1, "");
#ifdef __x86_64__
    char note[96];
    ksnprintf(note, sizeof(note), "%dx as fast as the kernel's allocator (times above)", rc - 100);
    report("faster than a system call per malloc", rc - 100 >= 2, note);
#else
    skip("faster than a system call per malloc", "the 32-bit kernel keeps the kernel's allocator");
#endif
}

static void t_avx(void) {
    section("AVX for apps (build 191)");
    char fp[64];
    fpu_describe(fp, sizeof(fp));
    if (!fpu_avx()) {
        skip("YMM registers kept across task switches", "no AVX on this processor (QEMU: CPU=max tools/dev.sh run)");
        terminal_write("        registers for apps: "); terminal_writeln(fp);
        return;
    }
    int rc = run("avx", NULL, NULL);
    report("YMM registers kept across task switches", rc == 0 || rc == 3, rc == 1 ? "a thread's registers changed" : fp);
    report("a new thread finds no other thread's AVX state", rc == 0, rc == 3 ? "old YMM halves were left" : "");
}

static void t_syscall(void) {
    section("system calls through syscall / sysret (build 192)");
#ifdef __x86_64__
    int rc = run("syscall", NULL, NULL);
    report("registers kept, a 9-argument call right", rc >= 100, rc >= 100 ? "" : "see the line above");
    if (rc < 100) return;
    char note[96];
    ksnprintf(note, sizeof(note), "%d.%dx as fast as int 0x80 (times above)", (rc - 100) / 10, (rc - 100) % 10);
    if (!idt_has_syscall()) skip("faster than int 0x80", "this processor has no syscall instruction: the stubs use int 0x80");
    else report("faster than int 0x80", rc - 100 >= 11, note);
#else
    skip("the syscall entry", "the 32-bit kernel calls the system directly (ring 0)");
#endif
}

/* ── disks: interrupt-driven waits (build 193) ── */
static volatile int g_tick_run, g_ticks;

/* tries to wake every millisecond while the disk is read */
static void disk_ticker(void) {
    while (g_tick_run == 1) { task_sleep_ms(1); g_ticks++; }
    g_tick_run = 2;
}

static uint32_t sum32(const uint8_t* b, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static void t_disk(void) {
    section("disks: waiting without stopping the system (build 193)");
    static ata_disk_t d[8];
    int n = disk_probe(d, 8), pick = -1;
    for (int i = 0; i < n && pick < 0; i++)
        if (d[i].present && !d[i].is_atapi && (d[i].nvme || d[i].vblk || d[i].ahci_port >= 0) && d[i].sectors >= 65536) pick = i;
    if (pick < 0) { skip("reads while other tasks run", "no SATA, virtio or NVMe disk (QEMU: DISK=disk.img, or -machine q35)"); return; }
    const char* kind = d[pick].nvme ? "NVMe" : d[pick].vblk ? "virtio" : "SATA";
    enum { CH = 2048, CHUNKS = 32 };                /* 1 MiB at a time, 32 MiB */
    uint8_t* buf = (uint8_t*)kmalloc(CH * 512);
    if (!buf) { report("reads while other tasks run", 0, "out of memory"); return; }
    uint32_t s0, w0, s1, w1;
    io_stats(&s0, &w0);
    uint32_t sums[2] = { 0, 0 }, ms[2] = { 0, 0 };
    int ok = 1;
    int concurrent_ticks = 0;
    for (int pass = 0; pass < 2 && ok; pass++) {
        g_ticks = 0;
        g_tick_run = pass == 0 ? 1 : 0;             /* (the first pass with the ticker beside it) */
        if (pass == 0 && task_create("disktick", disk_ticker) < 0) g_tick_run = 0;
        uint32_t t0 = timer_ms();
        for (int c = 0; c < CHUNKS && ok; c++) {
            if (disk_read(&d[pick], (uint32_t)c * CH, CH, buf) != 0) ok = 0;
            else sums[pass] = sums[pass] * 31u + sum32(buf, CH * 512);
        }
        ms[pass] = timer_ms() - t0;
        /* Capture progress before stopping/joining the ticker, and before
         * the comparison pass resets its counter. A wake during join is
         * not evidence of progress while the read loop was running. */
        if (pass == 0) concurrent_ticks = g_ticks;
        if (pass == 0 && g_tick_run == 1) { g_tick_run = 0; while (g_tick_run != 2) task_sleep_ms(1); }
    }
    kfree(buf);
    io_stats(&s1, &w1);
    char note[128];
    ksnprintf(note, sizeof(note), "%s disk, 32 MiB twice: %u ms, %u ms", kind, ms[0], ms[1]);
    report("the same data read twice", ok && sums[0] == sums[1], ok ? note : "a read failed");
    if (!ok) return;
    if (ms[0] < 20) {
        skip("other tasks run while it reads", "the reads took under 20 ms: nothing to measure");
    } else {
        ksnprintf(note, sizeof(note), "a 1 ms task woke %d times during the %u ms read/checksum loop", concurrent_ticks, ms[0]);
        /* This checks concurrent progress, not a wake-rate guarantee:
         * checksum work in the cooperative kernel cannot be preempted. */
        report("other tasks run while it reads", concurrent_ticks > 0, note);
    }
    if (d[pick].nvme) skip("woken by the disk's interrupt", "NVMe waits let the others run between looks (its interrupts stay off)");
    else {
        ksnprintf(note, sizeof(note), "%u waits slept, %u woken by the interrupt", s1 - s0, w1 - w0);
        if (s1 == s0) skip("woken by the disk's interrupt", "every command was done within the short spin");
        else report("woken by the disk's interrupt", w1 > w0, w1 > w0 ? note : "no interrupt arrived: 2 ms polling instead");
    }
}

/* ── the kernel's heap: bins (build 194) ── */
static void t_heap(void) {
    section("the kernel's heap (build 194)");
    char err[128] = "", note[128];
    int bad = kheap_check(err, sizeof(err));
    ksnprintf(note, sizeof(note), "%u free blocks, %u of %u KiB used", kheap_free_blocks(), kheap_used_bytes() >> 10, kheap_total_bytes() >> 10);
    report("the heap is sound (every block, list and bin)", !bad, bad ? err : note);
    enum { LIVE = 4000, PAIRS = 100000 };
    void** live = (void**)kmalloc(LIVE * sizeof(void*));
    if (!live) { report("malloc / free among many blocks", 0, "out of memory"); return; }
    uint32_t x = 12345;
    int fail = 0;
    for (int i = 0; i < LIVE; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        if (!(live[i] = kmalloc(x % 2000 + 16))) fail = 1;
    }
    for (int i = 0; i < LIVE; i += 2) { kfree(live[i]); live[i] = NULL; }
    uint32_t t0 = timer_ms();
    for (int i = 0; i < PAIRS && !fail; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        void* p = kmalloc(x % 4000 + 16);
        if (!p) fail = 1;
        kfree(p);
    }
    uint32_t ms = timer_ms() - t0;
    bad = kheap_check(err, sizeof(err));
    for (int i = 0; i < LIVE; i++) kfree(live[i]);
    kfree(live);
    if (!bad) bad = kheap_check(err, sizeof(err));
    ksnprintf(note, sizeof(note), "%d pairs among %d live blocks: %u ms (%u ns each)", PAIRS, LIVE / 2, ms,
              (uint32_t)((uint64_t)ms * 1000000u / PAIRS));
    report("malloc / free among many blocks", !fail && !bad && ms * 1000u < (uint32_t)PAIRS * 2u,
           fail ? "out of memory" : bad ? err : note);
}

/* ── the TLB: global kernel pages, PCIDs (build 195) ── */
static void t_tlb(void) {
    section("fewer TLB flushes: global kernel pages, PCIDs (build 195)");
#ifdef __x86_64__
    char msg[128] = "", note[128];
    uint32_t old_ns, new_ns;
    int kept;
    int rc = tlb_selftest(msg, sizeof(msg), &old_ns, &new_ns, &kept);
    ksnprintf(note, sizeof(note), "%s, %s", tlb_has_global() ? "global kernel pages" : "no global pages (no PGE)",
              tlb_has_pcid() ? "PCIDs" : "no PCIDs");
    report("a changed address space is never seen as it was", rc == 0, rc ? msg : note);
    if (rc) return;
    if (!tlb_has_pcid())
        skip("a space's translations kept across switches", "this processor has no PCIDs (QEMU: CPU=max, or KVM=1)");
    else
        report("a space's translations kept across switches", 1,
               kept ? "an untold change stayed unseen: the processor kept them"
                    : "the processor forgot them anyway (QEMU without KVM does)");
    if (old_ns && new_ns) {
        ksnprintf(note, sizeof(note), "there and back, 128 pages touched: %u ns, %u ns forgetting everything (%u.%ux)",
                  new_ns, old_ns, old_ns / new_ns, old_ns * 10u / new_ns % 10u);
        report("switches no slower", (uint64_t)new_ns * 4 <= (uint64_t)old_ns * 5, note);
    }
    int arc = run("tlb", NULL, NULL);
    tlb_stats_t st;
    tlb_stats(&st);
    ksnprintf(note, sizeof(note), "%d cores; since boot %llu of %llu loads kept the TLB, %llu full flushes, %u stale faults",
              arc - 100, st.kept, st.loads, st.full, uspace_stale_faults());
    report("threads remapping pages on every core: every fresh page zero", arc >= 100, arc >= 100 ? note : "see the line above");
#else
    skip("the TLB", "the 32-bit kernel runs without paging");
#endif
}

/* ── the terminal: scrollback, and what comes after it ── */
/* a line of a vt's text as a string (its trailing blanks cut) */
static void cells_str(const uint32_t* ch, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n && ch && o + 4 < cap; i++) o += u8_encode(ch[i] ? ch[i] : ' ', out + o);
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static void t_terminal(void) {
    section("the terminal: scrollback");
    int vt = terminal_vt_alloc(), was = terminal_vt_get_active();
    if (vt < 0) { skip("the terminal checks", "no free virtual terminal to write to"); return; }
    terminal_vt_set_size(vt, 20, 5);
    terminal_vt_set_active(vt);
    for (int i = 1; i <= 20; i++) {
        char l[16];
        ksnprintf(l, sizeof(l), "line %d\n", i);
        terminal_write(l);
    }
    terminal_vt_set_active(was);
    int hist = terminal_vt_history(vt);
    const uint32_t* hc;
    const uint8_t* hco;
    char newest[32] = "", oldest[32] = "";
    terminal_vt_history_line(vt, 1, &hc, &hco);
    cells_str(hc, 20, newest, sizeof(newest));
    terminal_vt_history_line(vt, hist, &hc, &hco);
    cells_str(hc, 20, oldest, sizeof(oldest));
    char note[96];
    ksnprintf(note, sizeof(note), "%d lines kept; the oldest \"%s\", the newest \"%s\"", hist, oldest, newest);
    /* 20 lines and an empty one under them in 5 rows: lines 1..16 scrolled off */
    report("lines that scroll off the top are kept", hist == 16 && !strcmp(oldest, "line 1") && !strcmp(newest, "line 16"), note);
    int moved = terminal_vt_set_view(vt, 10, hist);
    int at = terminal_vt_view(vt);
    terminal_vt_set_active(vt);
    terminal_write("x");
    terminal_vt_set_active(was);
    ksnprintf(note, sizeof(note), "10 lines back, then %d after new output", terminal_vt_view(vt));
    report("the view scrolls back, and new output brings it back", moved && at == 10 && terminal_vt_view(vt) == 0, note);

    /* ── ANSI escape sequences ── */
    section("the terminal: ANSI colours and cursor codes");
    const uint32_t* ch;
    const uint8_t* co;
    int w, h, stride;
    terminal_vt_get_buffer(vt, &ch, &co, &w, &h, &stride);
    #define TCELL(r, c) ch[(r) * stride + (c)]
    #define TCOL(r, c) co[(r) * stride + (c)]
    terminal_vt_set_active(vt);
    terminal_write("\x1b[2J\x1b[H\x1b[31mR\x1b[1;34mB\x1b[0mN\x1b[38;5;196mX\x1b[38;2;0;255;0mY\x1b[0;7mZ\x1b[0m");
    terminal_vt_set_active(was);
    int ok = TCELL(0, 0) == 'R' && TCOL(0, 0) == 0x04 && TCELL(0, 1) == 'B' && TCOL(0, 1) == 0x09 &&
             TCELL(0, 2) == 'N' && TCOL(0, 2) == 0x07 && TCOL(0, 3) == 0x0C && TCOL(0, 4) == 0x0A && TCOL(0, 5) == 0x70;
    ksnprintf(note, sizeof(note), "colours %02x %02x %02x %02x %02x %02x", TCOL(0, 0), TCOL(0, 1), TCOL(0, 2), TCOL(0, 3), TCOL(0, 4), TCOL(0, 5));
    report("colours: 16, bold, 256, 24-bit, reverse", ok, note);
    terminal_vt_set_active(vt);
    terminal_write("\x1b[2J\x1b[3;5Hq\x1b[1;1Habcdef\x1b[1;3H\x1b[K\x1b[2;1Hxyz\x1b[2;2H\x1b[P");
    terminal_vt_set_active(was);
    char row0[32], row1[32];
    cells_str(&TCELL(0, 0), w, row0, sizeof(row0));
    cells_str(&TCELL(1, 0), w, row1, sizeof(row1));
    ok = TCELL(2, 4) == 'q' && TCELL(2, 3) == ' ' && !strcmp(row0, "ab") && !strcmp(row1, "xz");
    ksnprintf(note, sizeof(note), "row 1 \"%s\", row 2 \"%s\", (3,5) '%c'", row0, row1, (char)TCELL(2, 4));
    report("cursor positioning, clearing the screen and a line, deleting a character", ok, note);
    int esc_left = 0;
    for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) if (TCELL(r, c) == 0x1B) esc_left = 1;
    terminal_vt_set_active(vt);
    terminal_write("\x1b]0;My title\x07\x1b[?25l");
    terminal_vt_set_active(was);
    int hidden = !terminal_vt_cursor_shown(vt);
    terminal_vt_set_active(vt);
    terminal_write("\x1b[?25h");
    terminal_vt_set_active(was);
    ok = !strcmp(terminal_vt_title(vt), "My title") && hidden && terminal_vt_cursor_shown(vt) && !esc_left;
    ksnprintf(note, sizeof(note), "title \"%s\", cursor hidden then shown%s", terminal_vt_title(vt), esc_left ? ", an ESC left on screen" : "");
    report("the title, hiding the cursor, nothing of the codes on screen", ok, note);
    static const struct { const char* seq; int shape; } cs[] = {
        { "\x1b[2 q", TERM_CURSOR_BLOCK }, { "\x1b[4 q", TERM_CURSOR_UNDERLINE }, { "\x1b[6 q", TERM_CURSOR_BAR },
        { "\x1b[1 qX", TERM_CURSOR_BLOCK }, { "\x1b[0 q", TERM_CURSOR_DEFAULT },
    };
    int csbad = -1;
    terminal_vt_set_active(vt);
    terminal_write("\x1b[2J\x1b[H");
    for (unsigned i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
        terminal_write(cs[i].seq);
        if (terminal_vt_cursor_shape(vt) != cs[i].shape && csbad < 0) csbad = (int)i;
    }
    terminal_vt_set_active(was);
    ok = csbad < 0 && TCELL(0, 0) == 'X' && TCELL(0, 1) == ' ';
    ksnprintf(note, sizeof(note), csbad < 0 ? "block, underline, bar, block, the default; only \"X\" drawn" : "step %d: shape %d", csbad,
              csbad < 0 ? 0 : terminal_vt_cursor_shape(vt));
    report("the cursor's shape (ESC [ n SP q)", ok, note);
    uint32_t bells = terminal_vt_bells(vt);
    terminal_vt_set_active(vt);
    terminal_write("\x1b[2J\x1b[Ha\ab");
    terminal_vt_set_active(was);
    int rang = terminal_vt_ringing(vt), drawn = TCELL(0, 0) == 'a' && TCELL(0, 1) == 'b';
    uint32_t t0 = timer_ms();
    while (timer_ms() - t0 < 250) task_yield();
    ok = terminal_vt_bells(vt) == bells + 1 && rang && !terminal_vt_ringing(vt) && drawn;
    ksnprintf(note, sizeof(note), "%u bell, flashing %d then %d, \"ab\" with nothing between: %d",
              terminal_vt_bells(vt) - bells, rang, terminal_vt_ringing(vt), drawn);
    report("BEL: a visual bell for 200 ms, nothing drawn", ok, note);
    #undef TCELL
    #undef TCOL
    terminal_vt_set_active(vt);
    terminal_write("\x1b[31m");                     /* (a colour, lines kept: left for the next owner) */
    for (int i = 0; i < 8; i++) terminal_write("more\n");
    terminal_vt_set_active(was);
    terminal_vt_free(vt);
    int again = terminal_vt_alloc();
    ok = again != vt || (!terminal_vt_title(again)[0] && !terminal_vt_history(again) && terminal_vt_cursor_shown(again));
    if (again == vt) {
        terminal_vt_set_active(again);
        terminal_write("n");
        terminal_vt_set_active(was);
        terminal_vt_get_buffer(again, &ch, &co, &w, &h, &stride);
        ok = ok && co[0] == co[1];                /* (the colour of a blank cell: not the red) */
    }
    ksnprintf(note, sizeof(note), again == vt ? "vt %d again: title \"%s\", %d lines kept, colour %02x" : "another vt (%d)",
              again, terminal_vt_title(again), terminal_vt_history(again), again == vt ? co[0] : 0);
    report("a terminal given out again starts clean (title, scrollback, colours)", ok, note);
    if (again >= 0) terminal_vt_free(again);

    /* ── colour schemes ── */
    section("the terminal: colour schemes");
    int had = termcolors_current(), sd = termcolors_find("solarized-DARK"), dr = termcolors_find("Dracula");
    uint32_t gen = termcolors_generation();
    termcolors_set(dr);
    char saved_name[32] = "";
    settings_get("term_colors", saved_name, sizeof(saved_name));
    ok = sd >= 0 && dr >= 0 && termcolors_current() == dr && termcolors_rgb(0) == 0x282A36 && termcolors_rgb(12) == 0xFF6E6E &&
         termcolors_generation() != gen && !strcmp(saved_name, "Dracula") && termcolors_find("no such") < 0;
    ksnprintf(note, sizeof(note), "%d schemes; Dracula's background %06x, saved as \"%s\"", termcolors_count(), termcolors_rgb(0), saved_name);
    termcolors_set(had);
    report("colour schemes: found by name, used, saved", ok, note);

    /* ── Tab completion ── */
    section("the terminal: Tab completion");
    static const char* const tc[][2] = {
        { "neof", "neofetch " }, { "ls /hom", "ls /home/" }, { "ls ~/Exam", "ls ~/Examples/" },
        { "cat ~/Examples/hel", "cat ~/Examples/hello.bpk " }, { "fs", "fs" },
    };
    int bad = -1;
    char got[160];
    for (unsigned i = 0; i < sizeof(tc) / sizeof(tc[0]) && bad < 0; i++) {
        shell_complete(tc[i][0], got, sizeof(got));
        if (strcmp(got, tc[i][1])) bad = (int)i;
    }
    if (bad >= 0) ksnprintf(note, sizeof(note), "\"%s\" became \"%s\", not \"%s\"", tc[bad][0], got, tc[bad][1]);
    else ksnprintf(note, sizeof(note), "a command, a folder, ~, a file; \"fs\" (fsck, fstest) left as it is");
    report("Tab completes commands and paths", bad < 0, note);

    /* echo -e: what programs' escape codes are tried with */
    static const char* const ec[][2] = {
        { "echo -e \"a\\tb\\e[31mc\\x41\\0102\\\\\"", "a\tb\x1b[31mcAB\\\n" },
        { "echo -n plain", "plain" }, { "echo \"a\\nb\"", "a\\nb\n" }, { "echo -ne x\\cy", "x" },
    };
    bad = -1;
    char eo[48] = "";
    for (unsigned i = 0; i < sizeof(ec) / sizeof(ec[0]) && bad < 0; i++) {
        if (!terminal_capture_start()) { bad = (int)i; break; }
        shell_exec(ec[i][0]);
        uint32_t n;
        char* out = terminal_capture_stop(&n);
        int same = out && n == strlen(ec[i][1]) && !memcmp(out, ec[i][1], n);
        if (!same) { bad = (int)i; if (out) { if (n >= sizeof(eo)) n = sizeof(eo) - 1; memcpy(eo, out, n); eo[n] = 0; } }
        kfree(out);
    }
    for (char* p = eo; *p; p++) if (*p == 0x1B) *p = '^'; else if ((unsigned char)*p < ' ') *p = '.';
    if (bad >= 0) ksnprintf(note, sizeof(note), "%s: \"%s\"", ec[bad][0], eo);
    else ksnprintf(note, sizeof(note), "-e: \\t \\e \\x41 \\0102 \\\\, -n, no -e: as typed, \\c");
    report("echo -e and -n", bad < 0, note);

    /* ── Ctrl+Shift+C: a terminal window's selection copied ── */
    section("the terminal: copy and paste");
    if (!gui_term_select(was, 0, 0, 0, 0, 0)) {
        skip("Ctrl+Shift+C copies the selection", "not in a desktop terminal window (startx, then selftest terminal)");
        return;
    }
    static char saved[4096];
    uint32_t slen, glen;
    const char* cur = clipboard_get(&slen);
    int keep = slen < sizeof(saved);
    uint32_t saved_len = slen;
    if (keep) memcpy(saved, cur, slen);
    terminal_write("  copy me: caf\xC3\xA9 \xF0\x9F\x8D\x8C\n");
    size_t pr, pc;
    terminal_get_cursor(&pr, &pc);
    int none = gui_term_copy(was);                  /* (no selection: Ctrl+C stays Ctrl+C) */
    gui_term_select(was, 1, (int)pr - 1, 2, (int)pr - 1, terminal_get_width() - 1);
    int copied = gui_term_copy(was);
    gui_term_select(was, 0, 0, 0, 0, 0);
    const char* got2 = clipboard_get(&glen);
    ok = !none && copied && !strcmp(got2, "copy me: caf\xC3\xA9 \xF0\x9F\x8D\x8C");
    ksnprintf(note, sizeof(note), "\"%s\"%s", got2, none ? ", copied with nothing selected" : "");
    report("Ctrl+Shift+C copies the selection, trailing blanks cut", ok, note);
    /* the selection keeps to its text when output pushes it up (into the scrollback) */
    terminal_write("  keep me selected\n");
    terminal_get_cursor(&pr, &pc);
    gui_term_select(was, 1, (int)pr - 1, 2, (int)pr - 1, terminal_get_width() - 1);
    for (int i = 0; i < terminal_get_height() + 3; i++) terminal_write("  ..\n");
    copied = gui_term_copy(was);
    gui_term_select(was, 0, 0, 0, 0, 0);
    got2 = clipboard_get(&glen);
    ok = copied && !strcmp(got2, "keep me selected");
    ksnprintf(note, sizeof(note), "\"%s\" after %d lines more", got2, (int)terminal_get_height() + 3);
    report("a selection moves up with its text", ok, note);
    if (keep) clipboard_set(saved, saved_len);

    /* Ctrl+= / Ctrl+- / Ctrl+0: the window's own text size */
    const uint32_t* zc;
    const uint8_t* zo;
    int w0, h0, zw, zh, zs;
    terminal_vt_get_buffer(was, &zc, &zo, &w0, &h0, &zs);
    int s0 = gui_term_text_size(was);
    gui_term_zoom(was, s0 == GFX_TEXT_SIZES - 1 ? -1 : 1);       /* (one bigger; at the biggest, one smaller) */
    int s1 = gui_term_text_size(was);
    terminal_vt_get_buffer(was, &zc, &zo, &zw, &zh, &zs);
    int fewer = s1 > s0 ? zw < w0 : zw > w0;
    gui_term_zoom(was, 0);
    int s2 = gui_term_text_size(was);
    int w2, h2;
    terminal_vt_get_buffer(was, &zc, &zo, &w2, &h2, &zs);
    ok = s1 != s0 && fewer && s2 == gfx_text_size() && w2 == w0 && h2 == h0;
    ksnprintf(note, sizeof(note), "size %d -> %d (%dx%d cells -> %dx%d), Ctrl+0: %d, %dx%d", s0, s1, w0, h0, zw, zh, s2, w2, h2);
    report("Ctrl+= / Ctrl+- / Ctrl+0: the window's text size", ok, note);

    /* Ctrl+Shift+F: two marks, one pushed into the scrollback, found newest first */
    char mark[24];
    ksnprintf(mark, sizeof(mark), "zq%u-mark", timer_ms() % 9000u + 1000u);  /* (not on screen before) */
    char line[48];
    ksnprintf(line, sizeof(line), "  %s one\n", mark);
    terminal_write(line);
    for (int i = 0; i < terminal_get_height() + 5; i++) terminal_write("  .\n");
    ksnprintf(line, sizeof(line), "    %s two\n", mark);
    terminal_write(line);
    gui_term_find_open(was);
    for (const char* p = mark; *p; p++) gui_term_find_key(was, *p >= 'a' && *p <= 'z' ? *p - 32 : *p);   /* (in capitals: any case) */
    int r1 = 0, c1 = 0, r2 = 0, c2 = 0, r3 = 0, c3 = 0;
    int f1 = gui_term_find_match(was, &r1, &c1);
    gui_term_find_key(was, GUI_FIND_OLDER);
    int f2 = gui_term_find_match(was, &r2, &c2), back = terminal_vt_view(was);
    gui_term_find_key(was, GUI_FIND_OLDER);              /* (no older one: it stays) */
    int f3 = gui_term_find_match(was, &r3, &c3);
    gui_term_find_key(was, 27);
    ok = f1 && c1 == 4 && f2 && c2 == 2 && r2 < 0 && back > 0 && f3 && r3 == r2 && !gui_term_finding(was) && !terminal_vt_view(was);
    ksnprintf(note, sizeof(note), "newest at row %d col %d, older at %d col %d (view %d lines back), Esc", r1, c1, r2, c2, back);
    report("Ctrl+Shift+F finds in the text and the scrollback", ok, note);

    /* Ctrl+click: the web address under the mouse */
    terminal_write("  see (https://en.wikipedia.org/wiki/Banana_(fruit)),\n  or HTTP://example.com/a?b=1.\n");
    size_t lr, lcol;
    terminal_get_cursor(&lr, &lcol);
    int row = (int)lr - 2;
    char u1[96] = "", u2[96] = "", u3[96] = "";
    int l1 = gui_term_link_at(was, row, 20, u1, sizeof(u1));      /* (inside the first) */
    int l2 = gui_term_link_at(was, row + 1, 10, u2, sizeof(u2));  /* (the second) */
    int l3 = gui_term_link_at(was, row, 3, u3, sizeof(u3));       /* ("see") */
    ok = l1 && !strcmp(u1, "https://en.wikipedia.org/wiki/Banana_(fruit)") && l2 && !strcmp(u2, "HTTP://example.com/a?b=1") && !l3;
    ksnprintf(note, sizeof(note), "\"%s\", \"%s\", none on a word: %d", u1, u2, !l3);
    report("Ctrl+click opens the web address under the mouse", ok, note);
}

/* ── neofetch's GPU lines: the real controllers, the active one named ── */
static void t_gpu_names(void) {
    gpu_name_t gn[4];
    int n = gpu_names(gn, 4);
    gpu_t* g = gpu_active();
    char note[120];
    if (!n) { skip("neofetch names the GPU", "no display controller on the PCI bus"); return; }
    int ok = !g || (gn[0].active && !strcmp(gn[0].name, g->name));
    ksnprintf(note, sizeof(note), "%s%s%s%s", gn[0].name, gn[0].driver ? " (" : "", gn[0].driver ? gn[0].driver : "", gn[0].driver ? ")" : "");
    report("neofetch names the GPU the screen shows, first", ok, note);
}

/* ── UTF-8 everywhere: terminal cells, drawing, VGA, FAT names ── */
static void t_utf8(void) {
    section("UTF-8: any character in the terminal, the editor and file names");
    /* the terminal: each UTF-8 character one cell, holding its code point */
    static const uint32_t want[] = { 'a', 0xE9, 0x20AC, 0x3A9, 0x416, 0x2500, 0x1F34C, 0xFFFD, 'z' };
    const char* text = "a\xC3\xA9\xE2\x82\xAC\xCE\xA9\xD0\x96\xE2\x94\x80\xF0\x9F\x8D\x8C\x80z";  /* (\x80: a stray byte) */
    int vt = terminal_vt_alloc(), was = terminal_vt_get_active();
    if (vt < 0) {
        skip("terminal cells hold any character", "no free virtual terminal to write to");
    } else {
        terminal_vt_set_active(vt);
        terminal_write(text);
        terminal_vt_set_active(was);
        const uint32_t* ch;
        const uint8_t* co;
        int w, h, stride;
        terminal_vt_get_buffer(vt, &ch, &co, &w, &h, &stride);
        int ok = ch != NULL, at = -1;
        for (unsigned i = 0; ok && i < sizeof(want) / sizeof(want[0]); i++) if (ch[i] != want[i]) { ok = 0; at = (int)i; }
        char note[96];
        if (ok) ksnprintf(note, sizeof(note), "a e-acute euro Omega Zhe box-line banana, a stray byte as U+FFFD");
        else ksnprintf(note, sizeof(note), "cell %d holds U+%04X", at, ch ? ch[at] : 0);
        report("terminal cells hold any character", ok, note);
        terminal_vt_free(vt);
    }
    /* drawing: the 8x8 font has Latin-1, DejaVu Sans Mono the rest */
    static const char* const drawn[] = { "\xE2\x82\xAC", "\xCE\xA9", "\xD0\x96", "\xE2\x94\x80" };
    int glyphs = 0;
    if (font_available(FONT_MONO)) {
        static uint32_t buf[32 * 32];
        for (unsigned g = 0; g < 4; g++) {
            memset(buf, 0, sizeof(buf));
            font_draw(buf, 32, 0, 0, 32, 32, FONT_MONO, 20, 4, 24, drawn[g], (uint32_t)strlen(drawn[g]), 0x00FFFFFFu, 0);
            int lit = 0;
            for (int i = 0; i < 32 * 32; i++) if (buf[i]) lit++;
            if (lit > 8) glyphs++;
        }
        report("beyond Latin-1 drawn with DejaVu Sans Mono (euro, Omega, Zhe, box line)", glyphs == 4, glyphs == 4 ? "" : "a glyph came out empty");
    } else {
        skip("beyond Latin-1 drawn with DejaVu Sans Mono", "the font is not loaded");
    }
    /* VGA text mode: code page 437 has some of it */
    int vga = terminal_cp437(0xE9) == (char)0x82 && terminal_cp437(0x2500) == (char)0xC4 &&
              terminal_cp437(0x3A9) == (char)0xEA && terminal_cp437(0x2591) == (char)0xB0 && terminal_cp437(0x1F34C) == '?';
    report("VGA text mode shows what code page 437 has (e-acute, box lines, Omega, shades)", vga, "");
    /* FAT32 / exFAT names are UTF-16: there and back, a surrogate pair included */
    uint16_t u16[16];
    char back[32];
    const char* name = "caf\xC3\xA9 \xF0\x9F\x8D\x8C";
    int n16 = u8_to_utf16(name, (int)strlen(name), u16, 16);
    u8_from_utf16(u16, n16, back, sizeof(back));
    uint16_t lone[2] = { 'x', 0xD800 };
    char lb[8];
    u8_from_utf16(lone, 2, lb, sizeof(lb));
    int conv = n16 == 7 && u16[3] == 0xE9 && u16[5] == 0xD83C && u16[6] == 0xDF4C && !strcmp(back, name) &&
               !strcmp(lb, "x\xEF\xBF\xBD");
    report("UTF-8 to UTF-16 and back (file names on FAT32 / exFAT)", conv, conv ? "" : "the name came back different");
}

/* ── the boot's timeline (boottime) ── */
static void t_boottime(void) {
    section("the boot's timeline (boottime)");
    static boot_step_t st[64];
    int n = boottime_steps(st, 64), ordered = 1, have_ready = 0, have_timer = 0;
    for (int i = 0; i < n; i++) {
        if (i && st[i].ns < st[i - 1].ns) ordered = 0;
        if (!strcmp(st[i].what, "ready")) have_ready = 1;
        if (!strcmp(st[i].what, "timer")) have_timer = 1;
    }
    char note[96];
    ksnprintf(note, sizeof(note), "%d steps, the first \"%s\"", n, n ? st[0].what : "-");
    report("every boot step recorded, from the kernel's start", n >= 10 && !strcmp(st[0].what, "kernel") && have_timer && have_ready, note);
    uint64_t total = n ? st[n - 1].ns : 0;
    ksnprintf(note, sizeof(note), "the kernel to \"%s\": %u ms", n ? st[n - 1].what : "-", (uint32_t)(total / 1000000u));
    report("steps in order, with times", ordered && total > 0, note);
    char msg[96];
    int agree = builtin_meta_check(msg, sizeof(msg));
    ksnprintf(note, sizeof(note), "%d packages: size, SHA-256 and manifest as the build found them", agree);
    report("the App Store's list made without unpacking every package", agree > 0, agree > 0 ? note : msg);
    int nf = pci_check_list(msg, sizeof(msg));
    ksnprintf(note, sizeof(note), "%d functions; %u scans since boot read the bus once", nf, pci_scan_count());
    report("PCI: the bus walked once, every probe after that from the list", nf > 0, nf > 0 ? note : msg);
    uint64_t before = boottime_before_ns();
    if (before) {
        ksnprintf(note, sizeof(note), "about %u ms", (uint32_t)(before / 1000000u));
        report("the time before the kernel (firmware, menu, loading) estimated", 1, note);
        uint64_t fw, menu, load, other;
        if (boottime_loader(&fw, &menu, &load, &other)) {
            ksnprintf(note, sizeof(note), "firmware %u, menu %u, loading %u, other %u ms", (uint32_t)(fw / 1000000u),
                      (uint32_t)(menu / 1000000u), (uint32_t)(load / 1000000u), (uint32_t)(other / 1000000u));
            report("the loader's part split (Banana Boot's timestamps)", fw + menu + load + other <= before + before / 50 + 1000000u, note);
        } else {
            skip("the loader's part split", "no timestamps from the loader (GRUB, or a TSC not reset at power-on)");
        }
    } else {
        skip("the time before the kernel", "the time stamp counter was not reset at power-on here");
    }
}

/* ── TCP: window scaling, SACK (build 196) ── */
static uint8_t net_byte(uint32_t i) { return (uint8_t)(i * 7u + (i >> 11)); }

/* bytes through a loopback connection (127.0.0.1 to itself), checked
 * byte for byte: 0 and *ms, or -1 and what went wrong in msg */
static int net_move(tcp_conn_t* s, tcp_conn_t* c, uint32_t total, uint32_t* ms, char* msg, int cap) {
    enum { CH = 16384 };
    uint8_t* buf = (uint8_t*)kmalloc(CH);
    if (!buf) { ksnprintf(msg, (size_t)cap, "out of memory"); return -1; }
    uint32_t sent = 0, got = 0, t0 = timer_ms();
    int rc = 0;
    while (got < total) {
        if (timer_ms() - t0 > 60000) { ksnprintf(msg, (size_t)cap, "only %u of %u bytes in 60 s", got, total); rc = -1; break; }
        uint32_t space = tcp_send_space(s);
        if (sent < total && space) {
            uint32_t n = total - sent < space ? total - sent : space;
            if (n > CH) n = CH;
            for (uint32_t k = 0; k < n; k++) buf[k] = net_byte(sent + k);
            if (tcp_send(s, buf, n, 0) != (int)n) { ksnprintf(msg, (size_t)cap, "tcp_send failed"); rc = -1; break; }
            sent += n;
        }
        int n = tcp_recv(c, buf, CH, 0);
        if (n > 0) {
            for (int k = 0; k < n && !rc; k++)
                if (buf[k] != net_byte(got + (uint32_t)k)) { ksnprintf(msg, (size_t)cap, "byte %u wrong", got + (uint32_t)k); rc = -1; }
            if (rc) break;
            got += (uint32_t)n;
            continue;
        }
        if (n != NET_ERR_TIMEOUT) { ksnprintf(msg, (size_t)cap, "tcp_recv: %s after %u bytes", net_strerror(n), got); rc = -1; break; }
        net_wait(1);
    }
    *ms = timer_ms() - t0;
    kfree(buf);
    return rc;
}

static void t_net(void) {
    section("TCP: window scaling and SACK (build 196)");
    if (!net_if()->dev) { skip("a loopback connection", "no network card (loopback needs one: QEMU has e1000)"); return; }
    int err;
    const uint16_t port = 47196;
    tcp_listener_t* l = tcp_listen(port, &err);
    if (!l) { report("a loopback connection", 0, net_strerror(err)); return; }
    tcp_conn_t* c = tcp_connect(IP4(127, 0, 0, 1), port, 3000, &err);
    tcp_conn_t* s = c ? tcp_accept(l, 1000) : NULL;
    if (!c || !s) {
        report("a loopback connection", 0, c ? "not accepted" : net_strerror(err));
        if (c) tcp_abort(c);
        tcp_unlisten(l);
        return;
    }
    tcp_info_t ci, si;
    tcp_info(c, &ci);
    tcp_info(s, &si);
    char note[128], msg[96];
    ksnprintf(note, sizeof(note), "window %u KiB (shift %u), SACK %s", ci.rcv_window >> 10, ci.rcv_ws, ci.sack ? "on" : "off");
    report("window scaling and SACK agreed", ci.wscale && si.wscale && ci.sack && si.sack && ci.rcv_window > 65535u &&
           si.snd_window == ci.rcv_window, note);
    uint32_t ms;
    const uint32_t MB = 1u << 20;
    int rc = net_move(s, c, 8 * MB, &ms, msg, sizeof(msg));
    ksnprintf(note, sizeof(note), "8 MiB in %u ms (%u MB/s)", ms, ms ? (uint32_t)(8ull * 1000 / ms) : 0);
    report("8 MiB through loopback, every byte right", rc == 0, rc ? msg : note);
    /* every 25th data segment lost: SACK says what got through, only the
     * holes go again */
    tcp_stats_t a, b;
    tcp_stats(&a);
    tcp_test_loss(25);
    rc = net_move(s, c, 2 * MB, &ms, msg, sizeof(msg));
    tcp_test_loss(0);
    tcp_stats(&b);
    ksnprintf(note, sizeof(note), "2 MiB in %u ms: %u lost, %u holes resent by SACK, %u fast, %u timeouts",
              ms, b.test_dropped - a.test_dropped, b.sack_rexmit - a.sack_rexmit, b.fast_rexmit - a.fast_rexmit, b.rto_rexmit - a.rto_rexmit);
    report("1 segment in 25 lost: every byte right, holes resent", rc == 0 && b.sack_rexmit + b.fast_rexmit > a.sack_rexmit + a.fast_rexmit,
           rc ? msg : note);
    tcp_abort(c);
    tcp_abort(s);
    tcp_unlisten(l);
}

/* Install the matching built-in test, even if an older package is on disk. */
static void t_opencl(void) {
    section("OpenCL barriers, shared memory and vectors");
    const char* tmp = "/tmp/cltest.bpk";
    char msg[160] = { 0 };
    if (write_ex(tmp, "cltest.bpk") < 0 ||
        pkg_install(tmp, msg, sizeof(msg)) != 0) {
        fs_delete(tmp, 0);
        report("install cltest", 0, msg);
        return;
    }
    fs_delete(tmp, 0);
    char* args[] = { "cltest" };
    int rc = pkg_run("cltest", 1, args, 0, msg, sizeof(msg));
    report("OpenCL kernel tests", rc == 0, rc < 0 ? msg : "see individual checks above");
}

/* posixtest (the POSIX layer for ported programs), from the kernel's copy */
static void t_posix(const char* part) {
    section("the POSIX layer: mmap (162), pthreads (163), sockets (164), signals (165), dlopen (166), epoll (172)");
    const char* tmp = "/tmp/posixtest.bpk";
    char msg[160] = { 0 };
    if (write_ex(tmp, "posixtest.bpk") < 0 ||
        pkg_install(tmp, msg, sizeof(msg)) != 0) {
        fs_delete(tmp, 0);
        report("install posixtest", 0, msg);
        return;
    }
    fs_delete(tmp, 0);
    char* args[] = { "posixtest", (char*)part };
    int rc = pkg_run("posixtest", part ? 2 : 1, args, 0, msg, sizeof(msg));
    report("POSIX checks", rc == 0, rc < 0 ? msg : "see the checks above");
}

static void t_gl3(const char* driver) {
#ifndef __x86_64__
    (void)driver; skip("Mesa OpenGL renderer", "64-bit apps only");
#else
    const char* tmp = "/tmp/gl3test.bpk"; char msg[160] = {0};
    if (write_ex(tmp, "gl3test.bpk") < 0 || pkg_install(tmp, msg, sizeof(msg))) {
        fs_delete(tmp, 0); report("install gl3test", 0, msg); return;
    }
    fs_delete(tmp, 0);
    char* args[] = {"gl3test", (char*)driver};
    int rc = pkg_run("gl3test", 2, args, 0, msg, sizeof(msg));
    report(driver, rc == 0, rc < 0 ? msg : "OpenGL shaders, rendered pixels and framebuffer checks above");
#endif
}

static void mouse_demo(void) {
    if(!gui_is_enabled()) { report("mouse capture demo",0,"startx first, then selftest mouse demo in a terminal");return; }
    const char* tmp="/tmp/mousetest.bpk";char msg[160]={0};
    if(write_ex(tmp, "mousetest.bpk") < 0 || pkg_install(tmp,msg,sizeof(msg))) {
        fs_delete(tmp,0);report("install mouse demo",0,msg);return;
    }
    fs_delete(tmp,0);char* args[]={"mousetest"};
    int rc=pkg_run("mousetest",1,args,1,msg,sizeof(msg));
    report("mouse demo launched",rc>=0,rc<0?msg:"click to look around; Escape releases; Q quits (interactive check)");
}

static void t_ram(void) {
    section("firmware RAM reporting (build 169)");
    bootmem_tests_run(report);
    uint32_t kb=sysinfo_get()->mem_kb;
    uint64_t managed=(hmem_total_bytes()>>10)+(kheap_total_bytes()>>10);
    char note[128];
    ksnprintf(note,sizeof(note),"%u MiB usable RAM; %llu MiB above 4 GiB managed for apps",
              kb/1024u,hmem_total_bytes()>>20);
    report("live firmware total covers the managed RAM",kb>0 && kb>=managed,note);
}
static void sleep_unlink(const char* path) { if(fs_find_file(path)>=0)fs_delete(path,0); }
static void t_sleep(int cycle) {
    section("ACPI S3 sleep (QEMU PC)");acpi_sleep_tests(report);
    char reason[160];int available=!acpi_s3_status(reason,sizeof(reason));
    if(!available){skip("ACPI S3 capability",reason);if(cycle)report("S3 sleep/resume cycle",0,reason);return;}
    report("ACPI S3 capability",1,reason);
    if(cycle) {
        unsigned before=acpi_s3_resumes();
        char worker[64],ready[80],stop[80],err[96]="test app did not become ready";
        ksnprintf(worker,sizeof(worker),"/tmp/s3-worker-%d",task_current_pid());
        ksnprintf(ready,sizeof(ready),"%s.ready",worker);ksnprintf(stop,sizeof(stop),"%s.stop",worker);
        sleep_unlink(worker);sleep_unlink(ready);sleep_unlink(stop);
        int started=0;
        if(!install()) {
            char* argv[]={"systest","suspend",worker};
            if(app_spawn("/apps/systest/app-" BANANA_ARCH,3,argv,err,sizeof(err))>=0) {
                uint32_t t=timer_ms();
                while(fs_find_file(ready)<0 && timer_ms()-t<3000)task_sleep_ms(10);
                int f=fs_find_file(ready);char text[8]={0};
                started=f>=0 && fs_read(f,0,text,5)==5 && !memcmp(text,"ready",5);
            }
        }
        report("user threads are live before suspend",started,started?"":err);
        volatile uint32_t marker[64];
        for(unsigned i=0;i<64;i++)marker[i]=i*2654435761u^0x53335241u;
        netdev_t* nic=net_if()->dev;char ifname[8]={0};
        if(nic)memcpy(ifname,nic->ifname,sizeof(ifname));
        terminal_writeln("Sleeping for five seconds; the RTC will wake this VM...");
        int rc=acpi_s3_suspend(5,reason,sizeof(reason));
        report("S3 sleep/resume cycle",!rc && acpi_s3_resumes()==before+1,reason);
        int intact=1;for(unsigned i=0;i<64;i++)if(marker[i]!=(i*2654435761u^0x53335241u))intact=0;
        report("memory on the suspended stack survives",intact,"");
        uint32_t ticks=timer_irq_count(),ms=timer_ms();timer_sleep_ms(20);
        report("timer interrupts and monotonic time resume",timer_irq_count()!=ticks && timer_ms()-ms>=20,"");
        if(nic)report("network interface stays registered after resume",net_if()->dev==nic && nic->present && !memcmp(ifname,nic->ifname,sizeof(ifname)),nic->ifname);
        fs_write_path(stop,"stop",4);uint32_t until=timer_ms();
        while(fs_find_file(worker)<0 && timer_ms()-until<5000)task_sleep_ms(10);
        int f=fs_find_file(worker);char text[8]={0};
        report("user threads retain their stacks and floating-point state",started && f>=0 && fs_read(f,0,text,2)==2 && !memcmp(text,"ok",2),"");
        sleep_unlink(worker);sleep_unlink(ready);sleep_unlink(stop);
    }
}

/* ── ZIP archives (kernel/zip.c, kernel/deflate.c) ─────────────────── */

static int same_file(const char* a, const char* b) {
    int ia = fs_find_file(a), ib = fs_find_file(b);
    if (ia < 0 || ib < 0) return 0;
    fs_file_t* fa = fs_get_file(ia);
    uint32_t na = fa ? fa->size : 0;
    char* copy = (char*)kmalloc(na ? na : 1);
    if (!copy) return 0;
    if (na) memcpy(copy, fa->content, na);
    fs_file_t* fb = fs_get_file(ib);
    int same = fb && fb->size == na && (!na || memcmp(copy, fb->content, na) == 0);
    kfree(copy);
    return same;
}

/* a stored archive of one entry named name, by hand (names zip_create would never write) */
static uint32_t raw_zip(uint8_t* z, const char* name, const char* data) {
    uint32_t nl = (uint32_t)strlen(name), dl = (uint32_t)strlen(data), crc = crc32_update(0, data, dl), p = 0;
    uint8_t h[46];
#define P16(o, v) (h[o] = (uint8_t)(v), h[(o) + 1] = (uint8_t)((v) >> 8))
#define P32(o, v) (P16(o, (v) & 0xFFFF), P16((o) + 2, (v) >> 16))
    memset(h, 0, 46); P32(0, 0x04034b50u); P16(4, 20); P32(14, crc); P32(18, dl); P32(22, dl); P16(26, nl);
    memcpy(z + p, h, 30); p += 30; memcpy(z + p, name, nl); p += nl; memcpy(z + p, data, dl); p += dl;
    uint32_t cd = p;
    memset(h, 0, 46); P32(0, 0x02014b50u); P16(4, 20); P16(6, 20); P32(16, crc); P32(20, dl); P32(24, dl); P16(28, nl);
    memcpy(z + p, h, 46); p += 46; memcpy(z + p, name, nl); p += nl;
    memset(h, 0, 46); P32(0, 0x06054b50u); P16(8, 1); P16(10, 1); P32(12, p - cd); P32(16, cd);
    memcpy(z + p, h, 22); p += 22;
#undef P16
#undef P32
    return p;
}

static void t_zip(void) {
    section("ZIP archives: zip / unzip, Files' Extract and Compress");
    if (fs_find_dir("/tmp/zt") >= 0) fs_delete("/tmp/zt", 1);
    fs_mkdir_p("/tmp/zt/src/sub/deep");
    /* text (compresses well), pseudo-random bytes (does not), an empty file */
    uint32_t tn = 60000, bn = 20000;
    char* t = (char*)kmalloc(tn);
    uint8_t* b = (uint8_t*)kmalloc(bn);
    if (!t || !b) { kfree(t); kfree(b); report("ZIP checks", 0, "out of memory"); return; }
    for (uint32_t i = 0; i < tn; i++) t[i] = "the quick brown banana jumps over the lazy kernel\n"[i % 50];
    uint32_t r = 12345;
    for (uint32_t i = 0; i < bn; i++) { r = r * 1103515245u + 12345u; b[i] = (uint8_t)(r >> 16); }
    fs_write_path("/tmp/zt/src/a.txt", t, tn);
    fs_write_path("/tmp/zt/src/b.bin", b, bn);
    fs_write_path("/tmp/zt/src/empty.txt", "", 0);
    fs_write_path("/tmp/zt/src/sub/c.txt", "hello from a folder\n", 20);
    fs_write_path("/tmp/zt/src/sub/deep/d.txt", t, 777);
    kfree(t);
    kfree(b);

    char err[FS_PATH_LEN + 80], note[160];
    zip_stats_t st;
    const char* in[] = { "/tmp/zt/src" };
    int rc = zip_create("/tmp/zt/t.zip", in, 1, 0, NULL, NULL, &st, err, sizeof(err));
    ksnprintf(note, sizeof(note), "%d files, %d folders, %u -> %u bytes%s%s", st.files, st.dirs, st.bytes, st.zbytes, rc ? ": " : "", rc ? err : "");
    report("zip a folder (5 files, 3 folders), compressed", rc == 0 && st.files == 5 && st.dirs == 3 && st.zbytes < st.bytes / 2, note);
    int n = zip_list("/tmp/zt/t.zip", NULL, NULL, err, sizeof(err));
    ksnprintf(note, sizeof(note), "%d entries", n);
    report("list it", n == 8, note);
    rc = zip_extract("/tmp/zt/t.zip", "/tmp/zt/out", 0, NULL, NULL, &st, err, sizeof(err));
    int same = same_file("/tmp/zt/src/a.txt", "/tmp/zt/out/src/a.txt") && same_file("/tmp/zt/src/b.bin", "/tmp/zt/out/src/b.bin") &&
               same_file("/tmp/zt/src/empty.txt", "/tmp/zt/out/src/empty.txt") && same_file("/tmp/zt/src/sub/c.txt", "/tmp/zt/out/src/sub/c.txt") &&
               same_file("/tmp/zt/src/sub/deep/d.txt", "/tmp/zt/out/src/sub/deep/d.txt");
    ksnprintf(note, sizeof(note), "%d files, %d failed%s%s", st.files, st.failed, err[0] ? ": " : "", err);
    report("extract it: every file the same, byte for byte", rc == 0 && st.files == 5 && !st.failed && same, note);
    rc = zip_extract("/tmp/zt/t.zip", "/tmp/zt/out", 0, NULL, NULL, &st, err, sizeof(err));
    int kept = st.skipped == 5 && st.files == 0;
    rc |= zip_extract("/tmp/zt/t.zip", "/tmp/zt/out", ZIP_OVERWRITE, NULL, NULL, &st, err, sizeof(err));
    report("again: files there are kept, or replaced with -o", rc == 0 && kept && st.files == 5, "");

    /* names that would land outside the folder */
    uint8_t* z = (uint8_t*)kmalloc(512);
    if (z) {
        uint32_t zl = raw_zip(z, "../evil.txt", "pwned");
        fs_write_path("/tmp/zt/evil1.zip", z, zl);
        zl = raw_zip(z, "/tmp/zt/abs.txt", "pwned");
        fs_write_path("/tmp/zt/evil2.zip", z, zl);
        zip_stats_t s1, s2;
        zip_extract("/tmp/zt/evil1.zip", "/tmp/zt/x/y", 0, NULL, NULL, &s1, err, sizeof(err));
        zip_extract("/tmp/zt/evil2.zip", "/tmp/zt/x/y", 0, NULL, NULL, &s2, err, sizeof(err));
        report("\"../\" and absolute names are refused", s1.skipped == 1 && s2.skipped == 1 && fs_find_file("/tmp/zt/x/evil.txt") < 0 &&
               fs_find_file("/tmp/zt/abs.txt") < 0 && fs_find_file("/tmp/zt/evil.txt") < 0, "");
        zl = raw_zip(z, "ok.txt", "fine data");
        fs_write_path("/tmp/zt/good.zip", z, zl);
        zip_extract("/tmp/zt/good.zip", "/tmp/zt/g", 0, NULL, NULL, &s1, err, sizeof(err));
        z[30 + 6] ^= 0x20;                                  /* "fine data" -> "Fine data": the CRC no longer matches */
        fs_write_path("/tmp/zt/bad.zip", z, zl);
        zip_extract("/tmp/zt/bad.zip", "/tmp/zt/b", 0, NULL, NULL, &s2, err, sizeof(err));
        report("a damaged file is caught by its CRC", s1.files == 1 && s2.failed == 1 && fs_find_file("/tmp/zt/b/ok.txt") < 0, err);
        kfree(z);
    }
    fs_write_path("/tmp/zt/not.zip", "just some text, not an archive", 30);
    report("a file that is not a ZIP is refused", zip_list("/tmp/zt/not.zip", NULL, NULL, err, sizeof(err)) < 0, err);
    fs_delete("/tmp/zt", 1);
}

/* ── the clipboard history (Win+V) ──────────────────────────────────── */

/* the entry holding s: its index, or -1 */
static int clip_find(const char* s) {
    char b[64];
    for (int i = 0; i < clipboard_hist_count(); i++)
        if (clipboard_hist_peek(i, b, sizeof(b), NULL, NULL) == 0 && !strcmp(b, s)) return i;
    return -1;
}

/* ── compressed memory: swap in RAM (kernel/zswap.c) ───────────────── */

#ifdef __x86_64__
static volatile uint32_t g_sw_pages, g_sw_done, g_sw_disk;
static volatile uint64_t g_sw_before, g_sw_after;

static uint64_t ram_used(void) { return (uint64_t)kheap_used_bytes() + hmem_used_bytes(); }

/* while systest waits: its pages compressed, and how much RAM that gave back */
static void swap_helper(void) {
    task_sleep_ms(2500);
    g_sw_before = ram_used();
    g_sw_pages = zswap_compress_now();
    g_sw_after = ram_used();
    g_sw_disk = zswap_disk_bytes() ? zswap_writeback(0xFFFFFFFFu) : 0;   /* (installed: on to the disk) */
    g_sw_done = 1;
}
#endif

/* each layout typed key by key: letters, Shift, AltGr, the dead accents */
#define KS 0x2A                     /* Shift down / up */
#define KSU 0xAA
#define KG 0xE0, 0x38               /* AltGr down / up */
#define KGU 0xE0, 0xB8
#define KEND 0xFF
static void t_keyboard(void) {
    section("keyboard layouts");
    static const struct { const char* layout; uint8_t k[24]; const char* want; } C[] = {
        { "EN (Default)", { 0x10, 0x11, 0x1A, KS, 0x03, KSU, KEND }, "qw[@" },
        { "FR", { 0x10, 0x11, 0x02, 0x03, 0x0B, 0x32, 0x33, 0x34, 0x35, 0x28, KEND }, "az&\xC3\xA9\xC3\xA0,;:!\xC3\xB9" },
        { "FR", { KS, 0x02, 0x03, 0x0A, 0x0B, KSU, KG, 0x0B, 0x12, 0x06, KGU, KEND }, "1290@\xE2\x82\xAC[" },
        { "FR", { 0x1A, 0x12, KS, 0x1A, KSU, 0x17, 0x1A, 0x39, KEND }, "\xC3\xAA\xC3\xAF^" },
        { "BE", { 0x07, 0x09, 0x35, KG, 0x03, KGU, 0x10, KEND }, "\xC2\xA7!=@a" },
        { "DE", { 0x15, 0x2C, 0x0C, 0x27, 0x28, 0x1A, KS, 0x04, KSU, KG, 0x10, KGU, KEND }, "zy\xC3\x9F\xC3\xB6\xC3\xA4\xC3\xBC\xC2\xA7@" },
        { "DE", { 0x0D, 0x12, KS, 0x0D, KSU, 0x1E, KEND }, "\xC3\xA9\xC3\xA0" },
        { "UK", { KS, 0x03, 0x04, 0x28, KSU, 0x2B, 0x56, KEND }, "\"\xC2\xA3@#\\" },
        { "US-Intl", { 0x28, 0x12, 0x28, 0x39, 0x28, 0x2E, KS, 0x07, KSU, 0x18, KEND }, "\xC3\xA9'\xC3\xA7\xC3\xB4" },
        { "ES", { 0x27, 0x28, 0x1E, 0x0D, KS, 0x0D, KSU, KEND }, "\xC3\xB1\xC3\xA1\xC2\xA1\xC2\xBF" },
        { "IT", { 0x1A, KS, 0x1A, KSU, KG, 0x27, KGU, 0x2B, KEND }, "\xC3\xA8\xC3\xA9@\xC3\xB9" },
        { "PT", { 0x27, 0x2B, 0x1E, 0x0D, KEND }, "\xC3\xA7\xC3\xA3\xC2\xAB" },
        { "fr_CH", { 0x15, 0x1A, KS, 0x1A, KSU, KG, 0x03, KGU, KEND }, "z\xC3\xA8\xC3\xBC@" },
        { "de_CH", { 0x1A, 0x27, 0x28, KEND }, "\xC3\xBC\xC3\xB6\xC3\xA4" },
        { "BEPO", { 0x10, 0x11, 0x12, 0x13, 0x1E, 0x1F, 0x20, 0x21, KG, 0x1E, KGU, KEND }, "b\xC3\xA9poauie\xC3\xA6" },
        { "FR", { 0x1D, 0x10, 0x9D, KEND }, "\x01" },                    /* Ctrl+A: by the letter on the key */
    };
    int n = (int)(sizeof(C) / sizeof(C[0])), bad = 0;
    char note[160] = "";
    for (int i = 0; i < n; i++) {
        int len = 0;
        while (len < 24 && C[i].k[len] != KEND) len++;
        char got[64];
        int rc = keyboard_test_type(C[i].layout, C[i].k, len, got, sizeof(got));
        if ((rc != 0 || strcmp(got, C[i].want)) && !bad++)
            ksnprintf(note, sizeof(note), "%s: got \"%s\", want \"%s\"", C[i].layout, rc ? "(no such layout)" : got, C[i].want);
    }
    if (!bad) ksnprintf(note, sizeof(note), "%d sequences on %d layouts", n, keyboard_layout_count());
    report("every layout types what its keys show", bad == 0, note);
    int all = keyboard_layout_count() >= 12;
    for (int i = 0; i < keyboard_layout_count(); i++) {
        const char* name = keyboard_layout_at(i, NULL);
        char got[8];
        if (keyboard_test_type(name, (const uint8_t[]){ 0x39 }, 1, got, sizeof(got)) != 0 || strcmp(got, " ")) all = 0;
    }
    report("each layout can be chosen by its name", all, keyboard_layouts_help());
}

/* the time zones' rules on known dates, local <-> UTC, the clock set by hand */
static void t_time(void) {
    section("time zones and the clock");
    char note[128];
    struct { const char* zone; int y, mo, d, h, mi, s; int want; } C[] = {
        { "Europe/Paris",      2026, 3, 29, 0, 59, 59, 3600 },   /* (UTC) the last second of winter */
        { "Europe/Paris",      2026, 3, 29, 1, 0, 0, 7200 },     /* summer from 01:00 UTC */
        { "Europe/Paris",      2026, 10, 25, 0, 59, 59, 7200 },
        { "Europe/Paris",      2026, 10, 25, 1, 0, 0, 3600 },
        { "America/New_York",  2026, 3, 8, 6, 59, 59, -18000 },  /* 02:00 EST */
        { "America/New_York",  2026, 3, 8, 7, 0, 0, -14400 },
        { "America/New_York",  2026, 11, 1, 5, 59, 59, -14400 }, /* 02:00 EDT */
        { "America/New_York",  2026, 11, 1, 6, 0, 0, -18000 },
        { "Australia/Sydney",  2026, 4, 4, 15, 59, 59, 39600 },  /* 03:00 AEDT, 5 April */
        { "Australia/Sydney",  2026, 4, 4, 16, 0, 0, 36000 },
        { "Australia/Sydney",  2026, 10, 3, 16, 0, 0, 39600 },   /* 02:00 AEST, 4 October */
        { "Pacific/Auckland",  2026, 9, 26, 14, 0, 0, 46800 },   /* 02:00 NZST, 27 September */
        { "America/Santiago",  2026, 7, 1, 12, 0, 0, -14400 },
        { "America/Santiago",  2026, 12, 1, 12, 0, 0, -10800 },
        { "Asia/Kolkata",      2026, 7, 1, 12, 0, 0, 19800 },
        { "Asia/Kathmandu",    2026, 1, 1, 12, 0, 0, 20700 },
    };
    int bad = 0, n = (int)(sizeof(C) / sizeof(C[0]));
    for (int k = 0; k < n; k++) {
        int z = tz_find(C[k].zone);
        int got = z < 0 ? -1 : tz_offset_at(z, rtc_make_time(C[k].y, C[k].mo, C[k].d, C[k].h, C[k].mi, C[k].s));
        if (got != C[k].want && !bad++)
            ksnprintf(note, sizeof(note), "%s %04d-%02d-%02d %02d:%02d: %d s, want %d", C[k].zone, C[k].y, C[k].mo, C[k].d, C[k].h, C[k].mi, got, C[k].want);
    }
    if (!bad) ksnprintf(note, sizeof(note), "%d instants in %d zones", n, 7);
    report("summer time starts and ends on the right second", bad == 0, note);

    int was = tz_use(tz_find("Europe/Paris"));
    uint32_t a = tz_to_utc(rtc_make_time(2026, 7, 1, 12, 0, 0));
    uint32_t g = tz_to_utc(rtc_make_time(2026, 3, 29, 2, 30, 0));     /* (does not exist: read as winter) */
    uint32_t o = tz_to_utc(rtc_make_time(2026, 10, 25, 2, 30, 0));    /* (twice: the first, summer's) */
    int ok = a == rtc_make_time(2026, 7, 1, 10, 0, 0) && g == rtc_make_time(2026, 3, 29, 1, 30, 0) &&
             o == rtc_make_time(2026, 10, 25, 0, 30, 0) && tz_to_local(a) == rtc_make_time(2026, 7, 1, 12, 0, 0);
    report("local time to UTC and back (summer, the skipped and the repeated hour)", ok, "");
    uint32_t u = rtc_now_utc(), l = rtc_now();
    int d = (int)(l - u) - tz_offset_at(tz_current(), u);
    ksnprintf(note, sizeof(note), "local %u, UTC %u", l, u);
    report("the clock: local time = UTC + the zone's offset", u && d >= -1 && d <= 1, note);

    /* by hand: a day on, then back (the clock chip itself is written) */
    uint32_t t0 = rtc_now_utc();
    int set = rtc_set_utc(t0 + 86400u);
    uint32_t t1 = rtc_now_utc();
    rtc_set_utc(t0 + (t1 - t0 - 86400u));
    ksnprintf(note, sizeof(note), "%d s on", (int)(t1 - t0));
    report("the clock set by hand, and back", set == 0 && t1 - t0 >= 86400u && t1 - t0 <= 86401u, note);
    char err[96];
    int r1 = date_set_text("30/02/2026", NULL, err, sizeof(err)), r2 = date_set_text(NULL, "25:00", err, sizeof(err));
    report("dates and times that do not exist are refused", r1 != 0 && r2 != 0, "");
    tz_use(was);
}

static void t_swap(void) {
    section("compressed memory: swap in RAM (build 182)");
#ifndef __x86_64__
    skip("compressed memory", "the 64-bit kernel's apps only (their mmap memory)");
#else
    zswap_stats_t a, b;
    zswap_stats(&a);
    g_sw_done = 0;
    task_create("swaptest", swap_helper);
    int rc = run("swap", "32", "5000");
    while (!g_sw_done) task_sleep_ms(50);
    zswap_stats(&b);
    char note[128];
    ksnprintf(note, sizeof(note), "%u pages of 8192 compressed", g_sw_pages);
    report("an app's idle pages compressed", g_sw_pages >= 6000, note);
    uint64_t back = g_sw_before > g_sw_after ? g_sw_before - g_sw_after : 0;
    ksnprintf(note, sizeof(note), "%u KiB of RAM given back", (uint32_t)(back >> 10));
    report("their RAM goes back to the system", back >= (16u << 20), note);
    ksnprintf(note, sizeof(note), "%u pages brought back", (uint32_t)(b.pages_in - a.pages_in));
    report("touched again: decompressed, every byte right", rc == 0 && b.pages_in - a.pages_in >= 6000, note);
    report("nothing left compressed once it unmapped", b.pages == a.pages, "");
    if (!zswap_disk_bytes()) skip("the swap area on the disk", "none on the live CD (an installed system has one)");
    else {
        ksnprintf(note, sizeof(note), "%u pages written, %u read back", g_sw_disk, (uint32_t)(b.disk_in - a.disk_in));
        report("compressed pages to the disk and back, every byte right", rc == 0 && g_sw_disk >= 3000 && b.disk_in - a.disk_in >= 3000, note);
        report("nothing left on the disk once it unmapped", b.disk_pages == a.disk_pages && b.disk_used == a.disk_used, "");
    }
#endif
}

static void t_clip(void) {
    section("clipboard history (Win+V)");
    /* (the user's own entries and clipboard are left as they were) */
    uint32_t keep_len = 0;
    const char* cur = clipboard_get(&keep_len);
    char* keep = (char*)kmalloc(keep_len + 1);
    if (keep) memcpy(keep, cur, keep_len);
    static const char* const T[3] = { "selftest clip one", "selftest clip two", "selftest clip three" };
    int before = clipboard_hist_count();
    for (int i = 0; i < 3; i++) clipboard_set(T[i], (uint32_t)strlen(T[i]));
    report("copies go to the top, the newest first", clip_find(T[2]) == 0 && clip_find(T[1]) == 1 && clip_find(T[0]) == 2, "");
    int n = clipboard_hist_count();
    clipboard_set(T[0], (uint32_t)strlen(T[0]));
    report("copying one again moves it up (no double)", clip_find(T[0]) == 0 && clipboard_hist_count() == n, "");
    char note[64];
    ksnprintf(note, sizeof(note), "%d entries before, %d now (25 at most)", before, clipboard_hist_count());
    report("the history keeps them", clipboard_hist_count() >= 3 && clipboard_hist_count() <= CLIP_HIST_MAX, note);
    int p = 0, i1 = clip_find(T[1]);
    clipboard_hist_pin(i1, 1);
    clipboard_hist_peek(clip_find(T[1]), NULL, 0, NULL, &p);
    int saved = fs_find_file("/etc/clipboard-pinned") >= 0;
    clipboard_hist_pin(clip_find(T[1]), 0);
    report("an entry can be pinned (and is saved)", p == 1 && saved, "");
    uint32_t len = 0;
    clipboard_hist_use(clip_find(T[2]));
    const char* c = clipboard_get(&len);
    report("choosing one makes it the clipboard", len == strlen(T[2]) && !memcmp(c, T[2], len) && clip_find(T[2]) == 0, "");
    for (int i = 0; i < 3; i++) clipboard_hist_delete(clip_find(T[i]));
    report("entries can be deleted", clip_find(T[0]) < 0 && clip_find(T[1]) < 0 && clip_find(T[2]) < 0, "");
    if (keep) {
        clipboard_set(keep, keep_len);                   /* the user's clipboard back */
        kfree(keep);
    }
}

int selftest_cmd(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : NULL;
    if (only && strcmp(only, "llvmpipe") && strcmp(only, "virgl") && strcmp(only, "gpu") && strcmp(only, "ring3") && strcmp(only, "memory") && strcmp(only, "ram") && strcmp(only, "sleep") && strcmp(only, "limits") && strcmp(only, "cores") && strcmp(only, "clock") && strcmp(only, "malloc") && strcmp(only, "avx") && strcmp(only, "syscall") && strcmp(only, "disk") && strcmp(only, "heap") && strcmp(only, "tlb") && strcmp(only, "net") && strcmp(only, "boottime") && strcmp(only, "utf8") && strcmp(only, "terminal") && strcmp(only, "ipv6") && strcmp(only, "opencl") && strcmp(only, "posix") && strcmp(only, "browser") && strcmp(only, "mouse") && strcmp(only, "boot") && strcmp(only, "zip") && strcmp(only, "clipboard") && strcmp(only, "swap") && strcmp(only, "time") && strcmp(only, "keyboard")) {
        terminal_writeln("usage: selftest [llvmpipe | virgl | gpu | ring3 | memory | ram | sleep [cycle] | limits | cores | clock | malloc | avx | syscall | disk | heap | tlb | net | boottime | utf8 | terminal | opencl | browser [swm] | mouse [demo] | boot | posix [part] | zip | clipboard | swap | time | keyboard | ipv6 [host [port]]]   (filesystems: fstest <mount point>)");
        return 1;
    }
    if(only && (!strcmp(only,"virgl") || !strcmp(only,"llvmpipe")) && argc!=2) { terminal_writeln("usage: selftest virgl | llvmpipe");return 1; }
    if(only && !strcmp(only,"gpu") && argc!=2) { terminal_writeln("usage: selftest gpu");return 1; }
    if(only && !strcmp(only,"mouse") && (argc>3 || (argc==3 && strcmp(argv[2],"demo")))) {
        terminal_writeln("usage: selftest mouse [demo]");return 1;
    }
    if(only && !strcmp(only,"boot") && argc!=2) { terminal_writeln("usage: selftest boot");return 1; }
    if(only && !strcmp(only,"ram") && argc!=2) { terminal_writeln("usage: selftest ram");return 1; }
    if(only && !strcmp(only,"sleep") && (argc>3 || (argc==3 && strcmp(argv[2],"cycle")))) { terminal_writeln("usage: selftest sleep [cycle]");return 1; }
    uint16_t ipv6_port = 80;
    if(only && !strcmp(only,"browser") && (argc>3 || (argc==3 && strcmp(argv[2],"swm")))) {
        terminal_writeln("usage: selftest browser [swm]   (swm: live https://swm.mxtpasontop.fr/)");return 1;
    }
    if (only && !strcmp(only, "ipv6") && argc > 3) {
        uint32_t n;
        int used = k_parse_u32(argv[3], &n);
        if (!used || argv[3][used] || !n || n > 65535 || argc > 4) {
            terminal_writeln("usage: selftest ipv6 [host [port]]"); return 1;
        }
        ipv6_port = (uint16_t)n;
    }
    g_ok = g_fail = 0;
    uint32_t t0 = timer_ms();
    terminal_writeln(only && !strcmp(only, "browser") ? (argc==3?"selftest: checking live swm.mxtpasontop.fr":"selftest: checking browser frameworks (offline)") :
                     only && !strcmp(only, "opencl") ? "selftest: checking OpenCL" :
                     only && !strcmp(only, "ipv6") ? "selftest: checking IPv6" :
                     only && !strcmp(only, "posix") ? "selftest: checking the POSIX layer" :
                     only && !strcmp(only, "ram") ? "selftest: checking RAM reporting" :
                     only && !strcmp(only, "zip") ? "selftest: checking ZIP archives" :
                     only && !strcmp(only, "clipboard") ? "selftest: checking the clipboard history" :
                     only && !strcmp(only, "swap") ? "selftest: checking compressed memory" :
                     only && !strcmp(only, "time") ? "selftest: checking time zones and the clock" :
                     only && !strcmp(only, "keyboard") ? "selftest: checking the keyboard layouts" :
                     only && !strcmp(only, "sleep") ? "selftest: checking ACPI S3" :
                     only && !strcmp(only, "clock") ? "selftest: checking the monotonic clock" :
                     only && !strcmp(only, "malloc") ? "selftest: checking the apps' heap" :
                     only && !strcmp(only, "avx") ? "selftest: checking AVX" :
                     only && !strcmp(only, "syscall") ? "selftest: checking the system call entry" :
                     only && !strcmp(only, "disk") ? "selftest: checking disk waits" :
                     only && !strcmp(only, "heap") ? "selftest: checking the kernel's heap" :
                     only && !strcmp(only, "tlb") ? "selftest: checking the TLB" :
                     only && !strcmp(only, "net") ? "selftest: checking TCP" :
                     only && !strcmp(only, "boottime") ? "selftest: checking the boot's timeline" :
                     only && !strcmp(only, "utf8") ? "selftest: checking UTF-8" :
                     only && !strcmp(only, "terminal") ? "selftest: checking the terminal" :
                     "selftest: checking the system (it runs the systest app a few times)");
    if(only && !strcmp(only,"llvmpipe")) {
        section("LLVMpipe CPU rendering (optional Mesa/LLVM libraries)");t_gl3("llvmpipe");
    } else if(only && !strcmp(only,"virgl")) {
        section("VirGL contexts and host resources"); gpu_virgl_tests(report);
        if (gpu_virgl_available()) t_gl3("virgl");
    } else if(only && !strcmp(only,"gpu")) {
        section("GPU presentation and diagnostics");gpu_tests(report);t_gpu_names();
    } else if(only && !strcmp(only,"sleep")) {
        t_sleep(argc==3);
    } else if(only && !strcmp(only,"ram")) {
        t_ram();
    } else if (only && !strcmp(only, "zip")) {
        t_zip();
    } else if (only && !strcmp(only, "clipboard")) {
        t_clip();
    } else if (only && !strcmp(only, "keyboard")) {
        t_keyboard();
    } else if (only && !strcmp(only, "time")) {
        t_time();
    } else if (only && !strcmp(only, "swap")) {
        if (install() == 0) t_swap();
    } else if(only && !strcmp(only,"boot")) {
        section("graphical boot menu rendering");bootmenu_tests_run(report);
    } else if(only && !strcmp(only,"mouse")) {
        section("mouse capture and relative movement");mouse_capture_tests(report);
        section("mouse: two pointing devices");mouse_source_tests(report);
        if(argc==3)mouse_demo();
    } else if (only && !strcmp(only, "browser")) {
        if(argc==3) {
            section("SuperWebManager live website checks");
            browser_site_test_run(report);
        } else {
            section("Tailwind CSS, React, Vue, Alpine, Astro and DOM observers");
            browser_test_run(report);
        }
    } else if (only && !strcmp(only, "net")) {
        t_net();
    } else if (only && !strcmp(only, "boottime")) {
        t_boottime();
    } else if (only && !strcmp(only, "utf8")) {
        t_utf8();
    } else if (only && !strcmp(only, "terminal")) {
        t_terminal();
    } else if (only && !strcmp(only, "opencl")) {
        t_opencl();
    } else if (only && !strcmp(only, "posix")) {
        t_posix(argc > 2 ? argv[2] : NULL);
    } else if (only && !strcmp(only, "ipv6")) {
        section("IPv6 TCP, UDP and toggle checks");
        ipv6test_run(argc > 2 ? argv[2] : NULL, ipv6_port, report);
    } else if (install() == 0) {
        if (!only || !strcmp(only, "ring3")) t_ring3();
        if (!only || !strcmp(only, "memory")) t_memory();
        if (!only || !strcmp(only, "limits")) t_limits();
        if (!only || !strcmp(only, "cores")) t_cores();
        if (!only || !strcmp(only, "clock")) t_clock();
        if (!only || !strcmp(only, "malloc")) t_malloc();
        if (!only || !strcmp(only, "avx")) t_avx();
        if (!only || !strcmp(only, "syscall")) t_syscall();
        if (!only || !strcmp(only, "disk")) t_disk();
        if (!only || !strcmp(only, "heap")) t_heap();
        if (!only || !strcmp(only, "tlb")) t_tlb();
        if (!only) t_net();
        if (!only) t_boottime();
        if (!only) t_utf8();
        if (!only) t_terminal();
        if (!only) {
            t_opencl();
            browser_test_run(report);
            gpu_tests(report);
            t_gpu_names();
            mouse_capture_tests(report);
            mouse_source_tests(report);
            bootmenu_tests_run(report);
            t_ram();
            t_sleep(0);
            t_posix(NULL);
            t_zip();
            t_clip();
            t_swap();
            t_time();
            t_keyboard();
            if (ipv6_enabled()) ipv6test_run(NULL, 80, report);
            else skip("IPv6", "off; selftest ipv6 runs temporary local checks");
        }
    }
    char line[96];
    ksnprintf(line, sizeof(line), "selftest: %d passed, %d failed (%u ms)", g_ok, g_fail, timer_ms() - t0);
    terminal_write_color(line, g_fail ? VGA_COLOR_LIGHT_RED : VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    terminal_putchar('\n');
    return 1;
}
