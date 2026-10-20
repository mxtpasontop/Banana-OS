/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include "i18n.h"
#include "gpu_virgl.h"
#include "appwin.h"
#include "audio.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "task.h"
#include "timer.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "rtc.h"
#include "tz.h"
#include "random.h"
#include "clipboard.h"
#include "serial.h"
#include "font8x8.h"
#include "paging.h"
#include "../net/http.h"
#include "../sdk/include/banana_api.h"
#include "smp.h"
#include "zswap.h"
#include "webview.h"
#include "font.h"
#include "pkg.h"
#include "repo.h"
#include "wallpaper.h"
#include "image.h"
#include "gamepad.h"
#include "umem.h"
#include "uspace.h"
#include "idt.h"
#include "uvm.h"

#ifdef __x86_64__
/* the SDK's heap tells kernel memory from mapped memory by this address */
_Static_assert(BANANA_VM_BASE == UVM_BASE, "BANANA_VM_BASE (banana_api.h) must be kernel/uvm.h's UVM_BASE");
#endif
#include "meminfo.h"
#include "hmem.h"
#include "upage.h"
#include "usock.h"
#include "../net/net.h"

/* files and sockets an app has open at once: descriptors 3..255 (the SDK's
 * own - pipes, eventfd, epoll - come after them, from 256: sdk/lib/ufd.c).
 * Java keeps every jar of its class path open: Minecraft ran out at 64 */
#define APP_FD_MAX  253
#define APP_STACK   (1u << 20)        /* 1 MiB */
#define APP_GUARD   (16u << 10)       /* unmapped pages under it (64-bit): an overflow faults */
#define STACK_CANARY 0xB4A4A5C0u      /* at the bottom of the stack: checked on every system call */
#define APP_KEYQ    32
#define APP_MAX_IMAGE (64u << 20)
#define APP_THREADS 64                /* extra threads per app (api->thread_create): a thread per core on big machines */
/* 64-bit: the kernel stack of each app thread - its system calls run there
 * (the app's own stack is in ring 3's reach), with a guard page under it */
#define APP_KSTACK  (128u << 10)

/* ── per-app state ─────────────────────────────────────────────────── */

/* a thread's signals: blocked and pending ones (bit sig - 1), its sigaltstack */
typedef struct {
    uint64_t  blocked, pending;
    uintptr_t alt_sp;
    uint64_t  alt_size;
    int       alt_flags;
} sigstate_t;

typedef struct {
    int      used;
    int      fidx;
    uint32_t pos;
    int      flags;
    usock_t* sock;            /* a socket (kernel/usock.c), not a file */
} afd_t;

/* a thread an app started: a task of its own running app code on its own stack */
typedef struct {
    int       used;
    int       pid;             /* its task, -1 once it ended */
    int     (*fn)(void*);
    void*     arg;
    uint8_t*  stack;           /* stack_size bytes (the app's memory), APP_GUARD under them */
    size_t    stack_size;
    int       own_stack;       /* the app gave it (thread_start): not freed here */
    int       detached;        /* its record goes as soon as it ends (no thread_join) */
    uint8_t*  kstack;          /* 64-bit: its system calls' stack (kstack_new) */
    uintptr_t saved_sp;        /* app_enter(): where app_leave() returns to */
    int       running;         /* in app code (between app_enter() and its return) */
    int       done;
    int       ret;
    sigstate_t sig;            /* its mask, signals sent to it, its alternate stack */
} athread_t;

typedef struct {
    int       used;
    int       id;              /* slot index: the window owner id */
    int       pid;             /* the task it runs in */
    int       own_task;        /* started with app_spawn() */
    char      name[32];
    uspace_t* us;              /* 64-bit: its page tables (kernel/uspace.c) */
    umem_t*   mem;             /* everything it has: code, stacks, malloc() (kernel/umem.c) */
    uintptr_t base;            /* where the ELF was loaded (crash reports: app+offset) */
    banana_entry_t entry;
    uint8_t*  guard;           /* APP_GUARD bytes, page-aligned */
    uint8_t*  stack;           /* APP_STACK bytes above the guard */
    int       guarded;
    uint8_t*  kstack;          /* 64-bit: the main thread's system calls' stack */
    banana_api_t* uapi;        /* 64-bit: the call table in its memory (system call stubs) */
    uintptr_t ret_stub;        /* ... and where its entry (or a thread) returns to */
    char*     clip;            /* clipboard_get()'s copy, in its memory */
    afd_t     fds[APP_FD_MAX];
    uintptr_t saved_sp;        /* the kernel stack app_enter() left: exit() returns there */
    int       exit_code;
    int       vt;              /* the terminal it was started from */
    int       has_term;
    int       keyq[APP_KEYQ];
    int       kq_head, kq_tail;
    int       esc;             /* ESC [ parsing in getchar() */
    int       handles_int;     /* called interrupted(): Ctrl+C no longer stops it */
    int       interrupted;
    uint32_t  last_breath;
    int       kill_req;        /* Task Manager: End task */
    int       running;         /* between app_enter() and its return */
    char      crash_msg[192];  /* why it crashed (app_fault), printed by run() */
    int       argc;
    char**    argv;
    uintptr_t code_lo, code_hi; /* where its code is: the timer may switch away from there */
    athread_t threads[APP_THREADS];
    int       exit_req;        /* a thread called exit() / crashed: every thread stops */
    int       exit_req_code;
    int       seq;             /* its process id (getpid): counts the apps started since boot */
    /* signals (version 19): what each does, where handlers start, the main
     * thread's mask and alternate stack, signals sent to the app itself */
    sigstate_t msig;
    uint8_t   sig_disp[65];    /* BANANA_SIG_* */
    uint64_t  sig_onstack;     /* bit sig - 1: on the alternate stack */
    uint64_t  sig_pending;     /* sent to the app: the first thread not blocking one takes it */
    uintptr_t sig_entry;       /* the SDK's entry(sig, info, uc) */
    uintptr_t sigret_stub;     /* ... returns there: sigreturn */
} app_proc_t;

static app_proc_t g_procs[APP_MAX];
static banana_api_t g_api;
static sigstate_t* sig_state(app_proc_t* p);     /* (the calling thread's signals) */


/* which app - and which of its threads - each task runs (by pid): every
 * system call asks (cur), on any core, so it is a lookup, not a search */
static int16_t g_pid_app[TASK_MAX];     /* app id + 1; 0: none */
static int8_t  g_pid_thr[TASK_MAX];     /* thread index + 1; 0: the app's main thread */

static void pid_set(int pid, int app, int thr) {
    if (pid < 0 || pid >= TASK_MAX) return;
    g_pid_thr[pid] = (int8_t)(thr + 1);
    g_pid_app[pid] = (int16_t)(app + 1);
}

static void pid_clear(int pid, int app) {
    if (pid < 0 || pid >= TASK_MAX || g_pid_app[pid] != app + 1) return;
    g_pid_app[pid] = 0;
    g_pid_thr[pid] = 0;
}

static app_proc_t* cur(void) {
    int pid = task_current_pid();
    if (pid < 0 || pid >= TASK_MAX || !g_pid_app[pid]) return NULL;
    app_proc_t* p = &g_procs[g_pid_app[pid] - 1];
    return p->used ? p : NULL;
}

/* the calling thread of app p, NULL for its main thread */
static athread_t* cur_thread(app_proc_t* p) {
    int pid = task_current_pid();
    if (!p || pid < 0 || pid >= TASK_MAX || !g_pid_thr[pid] || g_pid_app[pid] != p->id + 1) return NULL;
    athread_t* t = &p->threads[g_pid_thr[pid] - 1];
    return t->used && t->pid == pid ? t : NULL;
}

/* ends the calling app right away (exit(), Ctrl+C, closed twice) */
int  app_enter(int (*fn)(void*), void* arg, void* stack_top, uintptr_t* saved_sp);
void app_leave(uintptr_t saved_sp, int code) __attribute__((noreturn));

static void __attribute__((noreturn)) leave(app_proc_t* p, int code) {
    athread_t* t = cur_thread(p);
    if (t) {
        /* exit(), Ctrl+C, End task or a crash in a thread: the whole app
         * ends - its main thread and the other threads notice exit_req */
        if (!p->exit_req) { p->exit_req = 1; p->exit_req_code = code; }
        t->running = 0;
        app_leave(t->saved_sp, code);
    }
    p->exit_code = code;
    p->running = 0;
    app_leave(p->saved_sp, code);
}

/* a thread's return value (thread_join), or the end of the whole app */
static void __attribute__((noreturn)) leave_thread(app_proc_t* p, athread_t* t, int ret) {
    (void)p;
    t->running = 0;
    app_leave(t->saved_sp, ret);
}

static void key_push(app_proc_t* p, int c) {
    int next = (p->kq_head + 1) % APP_KEYQ;
    if (next == p->kq_tail) return;
    p->keyq[p->kq_head] = c;
    p->kq_head = next;
}

static int key_pop(app_proc_t* p) {
    if (p->kq_head == p->kq_tail) return 0;
    int c = p->keyq[p->kq_tail];
    p->kq_tail = (p->kq_tail + 1) % APP_KEYQ;
    return c;
}

static int focused(app_proc_t* p) {
    return p->has_term && gui_focused_vt() == p->vt;
}

/* Called from the API calls an app makes: lets the rest of the system
 * run now and then (tasks are cooperative), paints the terminal, and
 * notices Ctrl+C and windows closed for good. */
static void breathe(app_proc_t* p, int force) {
    if (!p) return;
    uint32_t now = timer_ms();
    if (p->exit_req) leave(p, p->exit_req_code);
    /* the 32-bit kernel has no guard pages: a smashed canary stops the app */
    if (!cur_thread(p) && *(volatile uint32_t*)p->stack != STACK_CANARY) {
        ksnprintf(p->crash_msg, sizeof(p->crash_msg), "%s: stack overflow - it used more than %u KiB of stack "
                  "(big local arrays? malloc them)", p->name, APP_STACK >> 10);
        *(volatile uint32_t*)p->stack = STACK_CANARY;
        leave(p, 139);
    }
    if (!force && now - p->last_breath < 20) return;
    p->last_breath = now;
    if (appwin_kill_requested(p->id) || p->kill_req) leave(p, 137);
    if (focused(p)) {
        char c = keyboard_try_getchar();
        if (c == 3) {
            p->interrupted = 1;
#ifdef __x86_64__
            /* a SIGINT handler (or SIG_IGN) takes Ctrl+C instead */
            if (p->sig_disp[2] == BANANA_SIG_CATCH) p->sig_pending |= 1ull << 1;
            else if (p->sig_disp[2] == BANANA_SIG_IGNORE) {}
            else
#endif
            if (!p->handles_int) { terminal_writeln("^C"); leave(p, 130); }
        } else if (c) {
            key_push(p, (unsigned char)c);
        }
    }
    gui_poll();                         /* paints, and yields to the other tasks */
    terminal_vt_set_active(p->vt);
}

/* ── memory ────────────────────────────────────────────────────────── */

static void* a_malloc(unsigned long size) {
    app_proc_t* p = cur();
    if (!p || size > (256ul << 20)) return NULL;
    return umem_alloc(p->mem, size);
}

static void a_free(void* ptr) {
    app_proc_t* p = cur();
    if (p && ptr) umem_free(p->mem, ptr);
}

static void* a_realloc(void* ptr, unsigned long size) {
    app_proc_t* p = cur();
    if (!p || size > (256ul << 20)) return NULL;
    return umem_realloc(p->mem, ptr, size);
}

/* memory of app `owner` (its window pictures: kernel/appwin.c) */
void* app_mem_alloc(int owner, size_t size) {
    if (owner < 0 || owner >= APP_MAX || !g_procs[owner].used) return NULL;
    return umem_alloc(g_procs[owner].mem, size);
}

void app_mem_free(int owner, void* ptr) {
    if (owner < 0 || owner >= APP_MAX || !g_procs[owner].used) return;
    umem_free(g_procs[owner].mem, ptr);
}

#ifdef __x86_64__
/* 1 if [ptr, ptr + n) is p's: its blocks, or what it mapped (kernel/uvm.h) */
static int own(app_proc_t* p, const void* ptr, size_t n) {
    return umem_owns(p->mem, ptr, n) || uvm_owns(p->us, ptr, n);
}

static int own_str(app_proc_t* p, const char* s) {
    return umem_owns_str(p->mem, s) || uvm_owns_str(p->us, s);
}
#endif

/* ── mmap (version 16) ─────────────────────────────────────────────────
 * 64-bit: reserved addresses, pages on first touch, protection (uvm.h).
 * 32-bit: no paging - zeroed app memory, protection ignored. */

static void* a_vm_map(void* addr, unsigned long len, int prot, int flags) {
    app_proc_t* p = cur();
    if (!p || !len) return NULL;
#ifdef __x86_64__
    return (void*)uvm_map(p->us, p->mem, (uintptr_t)addr, len, prot, flags);
#else
    (void)prot;
    if ((flags & UVM_FIXED) && addr) {                  /* only over memory it has: cleared */
        if (!umem_owns(p->mem, addr, len)) return NULL;
        memset(addr, 0, len);
        return addr;
    }
    if (len > (256ul << 20)) return NULL;
    void* m = umem_alloc_pages(p->mem, (len + 4095) & ~4095ul, 0);
    if (m) memset(m, 0, (len + 4095) & ~4095ul);
    return m;
#endif
}

static int a_vm_unmap(void* addr, unsigned long len) {
    app_proc_t* p = cur();
    if (!p || !addr) return -1;
#ifdef __x86_64__
    return uvm_unmap(p->us, p->mem, (uintptr_t)addr, len);
#else
    if (!umem_owns(p->mem, addr, len)) return -1;
    size_t whole = umem_pages_size(p->mem, addr);
    if (whole && len >= whole) umem_free_pages(p->mem, addr, 0);   /* a whole mapping goes */
    else memset(addr, 0, len);                          /* (part of one: it stays, cleared) */
    return 0;
#endif
}

static int a_vm_protect(void* addr, unsigned long len, int prot) {
    app_proc_t* p = cur();
    if (!p || !addr) return -1;
#ifdef __x86_64__
    /* its ordinary memory (malloc: a JIT making code runnable) is always
     * readable, writable and runnable: nothing to change */
    if (umem_owns(p->mem, addr, len ? len : 1)) return 0;
    return uvm_protect(p->us, (uintptr_t)addr, len, prot);
#else
    (void)prot;
    return umem_owns(p->mem, addr, len) ? 0 : -1;
#endif
}

/* kernel/idt.c, every page fault (any core, either mode): a page of the
 * running app's mmap region touched for the first time gets its memory */
int app_vm_fault(uintptr_t addr, uint32_t err, int from_user, int ints_on) {
    app_proc_t* p = cur();
    if (!p || !p->us) return 0;
    /* (it gives up - out of memory - only once 400 rounds in a row freed nothing) */
    for (int idle = 0; idle < 400; ) {
        uint32_t idx = 0;
        int r = uvm_fault(p->us, p->mem, addr, err, &idx);
        if (r != 2 && r != 3) return r == 1;
#ifdef __x86_64__
        /* the page is in the swap area on the disk (kernel/zswap.c): read in -
         * where the fault may wait (app code, or the kernel with interrupts
         * on), on the boot core, which drives the disks */
        if (!from_user && !ints_on) return 0;
        if (cpu_id() != 0) {
            if (!from_user) return 0;
            task_ap_go_home();
        }
        __asm__ volatile("sti");
        /* 2: the page is on the disk; 3: no memory for it - some is made (compressed,
         * written out) or, if none can be yet, a moment waited */
        int rc = r == 2 ? zswap_disk_in(idx) : (int)zswap_reclaim();
        if (r == 2 && rc == -2) rc = (int)zswap_reclaim() ? 0 : -2;   /* (no memory to read it into) */
        idle = (r == 2 ? rc == 0 : rc > 0) ? 0 : idle + 1;
        __asm__ volatile("cli");
        if (r == 2 && rc == -1) { klog("vm fault: disk read of %u failed\n", idx); return 0; }
#else
        (void)from_user; (void)ints_on;
        return 0;
#endif
    }
#ifdef __x86_64__
    {
        zswap_stats_t z;
        zswap_stats(&z);
        klog("app: out of memory - no page for %p (heap %u KiB free, high %llu KiB free, pool %llu/%llu KiB, "
             "%u compressed in %llu KiB, %u on the disk in %llu KiB, %u entries)\n", (void*)addr,
             (uint32_t)((kheap_total_bytes() - kheap_used_bytes()) >> 10),
             (unsigned long long)((hmem_total_bytes() - hmem_used_bytes()) >> 10),
             (unsigned long long)(upage_used_bytes() >> 10), (unsigned long long)(upage_held_bytes() >> 10),
             (uint32_t)z.pages, (unsigned long long)(z.bytes >> 10), z.disk_pages,
             (unsigned long long)(z.disk_used >> 10), z.entries);
    }
#endif
    return 0;
}

/* 1 if [ptr, ptr + n) is the calling app's memory (64-bit: what a system
 * call may touch on its behalf; the 32-bit kernel does not check) */
static int uok(const void* ptr, size_t n) {
#ifdef __x86_64__
    app_proc_t* p = cur();
    return p && own(p, ptr, n);
#else
    (void)ptr; (void)n;
    return 1;
#endif
}

static int ustr(const char* s) {
#ifdef __x86_64__
    app_proc_t* p = cur();
    return p && own_str(p, s);
#else
    (void)s;
    return 1;
#endif
}

/* ── console ───────────────────────────────────────────────────────── */

static long file_write(app_proc_t* p, int fd, const void* buf, unsigned long len);

static long a_write(int fd, const void* buf, unsigned long len) {
    app_proc_t* p = cur();
    if (!p) return -1;
    if (fd == 1 || fd == 2) {
        const char* s = (const char*)buf;
        char chunk[257];
        unsigned long i = 0;
        while (i < len) {
            int n = 0;
            while (i < len && n < 256) {
                char c = s[i++];
                if (c) chunk[n++] = c;
            }
            chunk[n] = 0;
            terminal_write(chunk);
        }
        breathe(p, 0);
        return (long)len;
    }
    return file_write(p, fd, buf, len);
}

/* the next key for the app (ESC sequences become BANANA_KEY_*), 0 if none */
static int next_key(app_proc_t* p) {
    for (;;) {
        if (focused(p)) {                       /* F-keys and media keys: events of their own */
            int f = KEYF_CODE(keyboard_take_fkey());
            if (f >= KEYF_F1 && f < KEYF_F1 + 12) return BANANA_KEY_F1 + (f - KEYF_F1);
            if (f == KEYF_PLAY) return BANANA_KEY_PLAY;
            if (f == KEYF_STOP) return BANANA_KEY_STOP;
            if (f == KEYF_NEXT) return BANANA_KEY_NEXT;
            if (f == KEYF_PREV) return BANANA_KEY_PREV;
        }
        int c = key_pop(p);
        if (!c && focused(p)) c = (unsigned char)keyboard_try_getchar();
        if (!c) return 0;
        if (c == 3) {
            p->interrupted = 1;
            if (!p->handles_int) { terminal_writeln("^C"); leave(p, 130); }
            return 3;
        }
        if (p->esc == 0 && c == 27) {
            /* an arrow key sends ESC [ X right away; a lone ESC is ESC */
            uint32_t t0 = timer_ms();
            int c2 = 0;
            while (!c2 && timer_ms() - t0 < 30) { c2 = key_pop(p); if (!c2) c2 = (unsigned char)keyboard_try_getchar(); }
            if (c2 != '[') { if (c2) key_push(p, c2); return 27; }
            int c3 = 0;
            t0 = timer_ms();
            while (!c3 && timer_ms() - t0 < 30) { c3 = key_pop(p); if (!c3) c3 = (unsigned char)keyboard_try_getchar(); }
            switch (c3) {
            case 'A': return BANANA_KEY_UP;
            case 'B': return BANANA_KEY_DOWN;
            case 'C': return BANANA_KEY_RIGHT;
            case 'D': return BANANA_KEY_LEFT;
            case 'H': return BANANA_KEY_HOME;
            case 'F': return BANANA_KEY_END;
            case 'P': return BANANA_KEY_DELETE;
            case 'I': return BANANA_KEY_PGUP;
            case 'G': return BANANA_KEY_PGDN;
            case '5': keyboard_try_getchar(); return BANANA_KEY_PGUP;
            case '6': keyboard_try_getchar(); return BANANA_KEY_PGDN;
            case '3': keyboard_try_getchar(); return BANANA_KEY_DELETE;
            default: continue;
            }
        }
        return c;
    }
}

static int a_getchar(void) {
    app_proc_t* p = cur();
    if (!p || !p->has_term) return -1;
    for (;;) {
        int c = next_key(p);
        if (c) return c;
        breathe(p, 1);
        if (appwin_kill_requested(p->id)) leave(p, 137);
        task_sleep_ms(10);
        terminal_vt_set_active(p->vt);
    }
}

static int a_trygetchar(void) {
    app_proc_t* p = cur();
    if (!p || !p->has_term) return 0;
    breathe(p, 0);
    return next_key(p);
}

static int a_readline(char* buf, int max) {
    app_proc_t* p = cur();
    if (!p || max <= 0) return -1;
    int n = 0;
    buf[0] = 0;
    for (;;) {
        int c = a_getchar();
        if (c < 0) return -1;
        if (c == '\n' || c == '\r') { terminal_putchar('\n'); break; }
        if (c == '\b' || c == 127) {
            if (n > 0) { n--; terminal_cursor_left(); terminal_putchar(' '); terminal_cursor_left(); }
            continue;
        }
        if (c < 32 || c > 126) continue;
        if (n < max - 1) { buf[n++] = (char)c; terminal_putchar((char)c); }
    }
    buf[n] = 0;
    return n;
}

static void a_set_color(int fg, int bg) { terminal_setcolor((uint8_t)(fg & 15), (uint8_t)(bg & 15)); }
static void a_clear(void) { terminal_clear(); }
static void a_term_size(int* c, int* r) {
    if (c) *c = (int)terminal_get_width();
    if (r) *r = (int)terminal_get_height();
}
static void a_set_cursor(int r, int c) {
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    terminal_set_cursor((size_t)r, (size_t)c);
}

static int a_interrupted(void) {
    app_proc_t* p = cur();
    if (!p) return 0;
    p->handles_int = 1;
    breathe(p, 0);
    int r = p->interrupted;
    p->interrupted = 0;
    return r;
}

/* ── files ─────────────────────────────────────────────────────────── */

static afd_t* get_fd(app_proc_t* p, int fd) {
    if (!p || fd < 3 || fd >= APP_FD_MAX + 3) return NULL;
    afd_t* f = &p->fds[fd - 3];
    if (!f->used) return NULL;
    if (f->sock) return f;
    fs_file_t* ff = fs_file_info(f->fidx);
    if (!ff || !ff->used) return NULL;         /* deleted meanwhile */
    return f;
}

static int a_open(const char* path, int flags) {
    app_proc_t* p = cur();
    if (!p || !path || !*path) return -1;
    if (fs_find_dir(path) >= 0) return -1;
    int idx = fs_find_file(path);
    if (idx < 0) {
        if (!(flags & BANANA_O_CREATE)) return -1;
        idx = fs_create(path);
        if (idx < 0) return -1;
    } else if ((flags & BANANA_O_TRUNC) && (flags & BANANA_O_WRITE)) {
        if (fs_write(idx, "", 0) != 0) return -1;
    }
    for (int i = 0; i < APP_FD_MAX; i++) {
        if (p->fds[i].used) continue;
        p->fds[i].used = 1;
        p->fds[i].fidx = idx;
        p->fds[i].sock = NULL;
        p->fds[i].flags = flags ? flags : BANANA_O_READ;
        p->fds[i].pos = (flags & BANANA_O_APPEND) ? fs_file_info(idx)->size : 0;
        fs_pin(idx);                       /* (open: its entry stays, kernel/fs.c gives back no folder holding it) */
        return i + 3;
    }
    return -1;
}

static long a_read(int fd, void* buf, unsigned long len) {
    app_proc_t* p = cur();
    if (fd == 0) {                          /* stdin: a line from the terminal */
        if (!p || !len) return 0;
        char* b = (char*)buf;
        int n = a_readline(b, (int)(len > 1024 ? 1024 : len));
        if (n < 0) return 0;
        if ((unsigned long)n < len) b[n++] = '\n';
        return n;
    }
    afd_t* f = get_fd(p, fd);
    if (!f) return -1;
    if (f->sock) return usock_recv(f->sock, buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len, 0, NULL, NULL);
    /* the part asked for only: a big file on the disk is not read whole */
    int n = fs_read(f->fidx, f->pos, buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len);
    if (n < 0) return -1;
    f->pos += (uint32_t)n;
    breathe(p, 0);
    return (long)n;
}

static long file_write(app_proc_t* p, int fd, const void* buf, unsigned long len) {
    afd_t* f = get_fd(p, fd);
    if (f && f->sock) return usock_send(f->sock, buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len, 0, NULL, 0);
    if (!f || !(f->flags & (BANANA_O_WRITE | BANANA_O_APPEND))) return -1;
    fs_file_t* ff = fs_file_info(f->fidx);             /* (its data is not read in for a write) */
    if (!ff || !ff->used) return -1;
    if (f->flags & BANANA_O_APPEND) f->pos = ff->size;
    if (len > 0xFFFFFFFFul - f->pos) return -1;
    if (fs_write_at(f->fidx, f->pos, buf, (uint32_t)len) != 0) return -1;
    f->pos += (uint32_t)len;
    breathe(p, 0);
    return (long)len;
}

static long a_seek(int fd, long off, int whence) {
    afd_t* f = get_fd(cur(), fd);
    if (!f || f->sock) return -1;
    long base = whence == BANANA_SEEK_CUR ? (long)f->pos : whence == BANANA_SEEK_END ? (long)fs_file_info(f->fidx)->size : 0;
    long np = base + off;
    if (np < 0) return -1;
    f->pos = (uint32_t)np;
    return np;
}

static int a_close(int fd) {
    afd_t* f = get_fd(cur(), fd);
    app_proc_t* p = cur();
    if (!f) {
        if (p && fd >= 3 && fd < APP_FD_MAX + 3) p->fds[fd - 3].used = 0;
        return -1;
    }
    if (f->sock) { usock_close(f->sock); f->sock = NULL; }
    else if (f->fidx >= 0) fs_unpin(f->fidx);
    f->used = 0;
    return 0;
}

/* ── sockets (version 18, kernel/usock.c) ──────────────────────────────
 * In the app's file descriptors, so read / write / close work on them.
 * Errors are negative errno numbers. */

#define SOCK_NONBLOCK_F 04000
#define SOCK_CLOEXEC_F  02000000

/* waiting in a socket call: the app can be stopped, the desktop paints */
static void sock_breathe(void) { breathe(cur(), 1); }

static usock_t* sock_of(int fd) {
    afd_t* f = get_fd(cur(), fd);
    return f ? f->sock : NULL;
}

/* a descriptor for socket s: its number, or -EMFILE (s closed) */
static int sock_fd(app_proc_t* p, usock_t* s) {
    for (int i = 0; i < APP_FD_MAX; i++) {
        if (p->fds[i].used) continue;
        memset(&p->fds[i], 0, sizeof(afd_t));
        p->fds[i].used = 1;
        p->fds[i].fidx = -1;
        p->fds[i].flags = BANANA_O_READ | BANANA_O_WRITE;
        p->fds[i].sock = s;
        return i + 3;
    }
    usock_close(s);
    return -24;                                         /* EMFILE */
}

static int a_sock_open(int family, int type, int proto) {
    app_proc_t* p = cur();
    (void)proto;
    if (!p) return -22;
    usock_set_wait(sock_breathe);
    int err = 0;
    usock_t* s = usock_new(family, type & ~(SOCK_NONBLOCK_F | SOCK_CLOEXEC_F), &err);
    if (!s) return err;
    if (type & SOCK_NONBLOCK_F) usock_set_nonblock(s, 1);
    return sock_fd(p, s);
}

static int a_sock_bind(int fd, const void* sa, int len) {
    usock_t* s = sock_of(fd);
    return s ? usock_bind(s, sa, len) : -88;            /* ENOTSOCK */
}

static int a_sock_listen(int fd, int backlog) {
    usock_t* s = sock_of(fd);
    return s ? usock_listen(s, backlog) : -88;
}

static int a_sock_accept(int fd, void* sa, int* len) {
    app_proc_t* p = cur();
    usock_t* s = sock_of(fd);
    if (!p || !s) return -88;
    if (len && !uok(len, sizeof(int))) return -14;      /* EFAULT */
    if (sa && (!len || *len < 0 || !uok(sa, (size_t)*len))) return -14;
    int err = 0;
    usock_t* n = usock_accept(s, sa, len, &err);
    return n ? sock_fd(p, n) : err;
}

static int a_sock_connect(int fd, const void* sa, int len) {
    usock_t* s = sock_of(fd);
    return s ? usock_connect(s, sa, len) : -88;
}

static long a_sock_send(int fd, const void* buf, unsigned long len, int flags, const void* sa, int salen) {
    usock_t* s = sock_of(fd);
    if (!s) return -88;
    if (sa && (salen < 0 || !uok(sa, (size_t)salen))) return -14;
    return usock_send(s, buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len, flags, sa, salen);
}

static long a_sock_recv(int fd, void* buf, unsigned long len, int flags, void* sa, int* salen) {
    usock_t* s = sock_of(fd);
    if (!s) return -88;
    if (salen && !uok(salen, sizeof(int))) return -14;
    if (sa && (!salen || *salen < 0 || !uok(sa, (size_t)*salen))) return -14;
    return usock_recv(s, buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len, flags, sa, salen);
}

static int a_sock_shutdown(int fd, int how) {
    usock_t* s = sock_of(fd);
    return s ? usock_shutdown(s, how) : -88;
}

static int a_sock_opt(int fd, int set, int level, int name, void* val, int* len) {
    usock_t* s = sock_of(fd);
    if (!s) return -88;
    if (!len || !uok(len, sizeof(int)) || *len < 0 || (val && !uok(val, (size_t)*len))) return -14;
    return usock_opt(s, set, level, name, val, len);
}

static int a_sock_name(int fd, int peer, void* sa, int* len) {
    usock_t* s = sock_of(fd);
    if (!s) return -88;
    if (!len || !uok(len, sizeof(int)) || *len < 0 || (sa && !uok(sa, (size_t)*len))) return -14;
    return usock_name(s, peer, sa, len);
}

/* op 1: non-blocking on (arg 1) / off; 2: is it?; 3: bytes waiting (FIONREAD) */
static int a_sock_ctl(int fd, int op, int arg) {
    usock_t* s = sock_of(fd);
    if (!s) return -88;
    if (op == 1) { usock_set_nonblock(s, arg); return 0; }
    if (op == 2) return usock_nonblock(s);
    if (op == 3) return usock_nread(s);
    return -22;
}

/* poll(): struct pollfd { int fd; short events, revents; } x n; how many
 * are ready, 0 after timeout_ms (-1: no limit) */
typedef struct { int fd; short events, revents; } apollfd_t;
#define APOLLIN   0x1
#define APOLLOUT  0x4
#define APOLLNVAL 0x20

static int a_poll(void* fds, int n, int timeout_ms) {
    app_proc_t* p = cur();
    if (!p || n < 0 || n > 4096 || (n && !uok(fds, (size_t)n * sizeof(apollfd_t)))) return -22;
    apollfd_t* pf = (apollfd_t*)fds;
    uint32_t start = timer_ms();
    for (;;) {
        int ready = 0;
        for (int i = 0; i < n; i++) {
            int fd = pf[i].fd, ev = pf[i].events, r = 0;
            if (fd < 0) { pf[i].revents = 0; continue; }
            if (fd == 0) r = p->kq_head != p->kq_tail ? APOLLIN : 0;
            else if (fd <= 2) r = APOLLOUT;
            else {
                afd_t* f = get_fd(p, fd);
                if (!f) r = APOLLNVAL;
                else if (f->sock) r = usock_poll(f->sock, ev);
                else r = (APOLLIN | APOLLOUT) & ev;     /* (a file is always ready) */
            }
            pf[i].revents = (short)r;
            if (r) ready++;
        }
        if (ready || !timeout_ms) return ready;
        if (timeout_ms > 0 && timer_ms() - start >= (uint32_t)timeout_ms) return 0;
        net_wait(10);
        breathe(p, 1);
    }
}

/* a host name (or a dotted address, or localhost): its IPv4 address in
 * ip[4] (network order); 0, or -1 if it could not be found */
static int a_resolve(const char* name, unsigned char* ip) {
    if (!name || !ip || !uok(ip, 4)) return -1;
    ip4_t a = 0;
    if (dns_resolve(name, &a, 5000) != NET_OK) return -1;   /* (dotted and localhost too) */
    ip[0] = (unsigned char)(a >> 24); ip[1] = (unsigned char)(a >> 16);
    ip[2] = (unsigned char)(a >> 8);  ip[3] = (unsigned char)a;
    return 0;
}

static int a_remove(const char* path) {
    if (!path || !*path) return -1;
    if (fs_find_file(path) >= 0) { fs_delete(path, 0); return fs_find_file(path) >= 0 ? -1 : 0; }
    if (fs_find_dir(path) < 0) return -1;
    int d[1], f[1];
    if (fs_list_dirs(path, d, 1) > 0 || fs_list_files(path, f, 1) > 0) return -1;   /* not empty */
    fs_delete(path, 1);
    return fs_find_dir(path) >= 0 ? -1 : 0;
}

static int a_mkdir(const char* path) { return path && fs_mkdir(path) >= 0 ? 0 : -1; }
static int a_rename(const char* a, const char* b) { return a && b && fs_move(a, b) >= 0 ? 0 : -1; }

static int a_stat(const char* path, banana_stat_t* st) {
    if (!path || !st) return -1;
    int f = fs_find_file(path);
    if (f >= 0) { st->size = fs_file_info(f)->size; st->is_dir = 0; return 0; }
    if (fs_find_dir(path) >= 0) { st->size = 0; st->is_dir = 1; return 0; }
    return -1;
}

static int a_readdir(const char* path, int index, banana_dirent_t* ent) {
    if (!path || !ent || index < 0) return -1;
    /* (from the heap for the call: apps on several cores may list at once) */
    int* idx = (int*)kmalloc(FS_LIST_MAX * sizeof(int));
    if (!idx) return -1;
    int rc = -1;
    int nd = fs_list_dirs(path, idx, FS_LIST_MAX);
    if (nd >= 0 && index < nd && index < FS_LIST_MAX) {
        const fs_dir_t* d = fs_get_dir(idx[index]);
        if (d) {
            kstrlcpy(ent->name, d->name, sizeof(ent->name));
            ent->size = 0;
            ent->is_dir = 1;
            rc = 0;
        }
    } else if (nd >= 0) {
        index -= nd;
        int nf = fs_list_files(path, idx, FS_LIST_MAX);
        if (index < nf && index < FS_LIST_MAX) {
            fs_file_t* f = fs_file_info(idx[index]);
            kstrlcpy(ent->name, f->name, sizeof(ent->name));
            ent->size = f->size;
            ent->is_dir = 0;
            rc = 0;
        }
    }
    kfree(idx);
    return rc;
}

static int a_getcwd(char* buf, int size) {
    if (!buf || size <= 0) return -1;
    fs_cwd_path(buf, size);
    return 0;
}

static int a_chdir(const char* path) {
    if (!path || fs_find_dir(path) < 0) return -1;
    fs_cd(path);
    return 0;
}

/* ── time ──────────────────────────────────────────────────────────── */

static unsigned int a_ticks(void) { return timer_ms(); }

static int sig_waiting(app_proc_t* p);

static void a_sleep(unsigned int ms) {
    app_proc_t* p = cur();
    uint32_t end = timer_ms() + ms;
    breathe(p, 1);
    for (;;) {
        int32_t left = (int32_t)(end - timer_ms());
        if (left <= 0) break;
        task_sleep_ms(left > 10 ? 10 : (uint32_t)left);
        if (p) terminal_vt_set_active(p->vt);
        /* awake at the time asked: back to the app now - breathing first
         * (it yields) would hand the processor to the busy tasks again */
        if ((int32_t)(end - timer_ms()) <= 0) break;
        breathe(p, 0);                      /* (every 20 ms: Ctrl+C, End task, painting) */
        if (p && sig_waiting(p)) break;     /* (a signal ends the sleep, as EINTR does) */
    }
}

static void a_yield(void) { breathe(cur(), 1); }

static void a_localtime(banana_time_t* t) {
    rtc_datetime_t dt;
    memset(t, 0, sizeof(*t));
    if (rtc_read_datetime(&dt) != 0) { t->year = 2026; t->month = 1; t->day = 1; return; }
    t->year = dt.year; t->month = dt.month; t->day = dt.day;
    t->hour = dt.hour; t->minute = dt.minute; t->second = dt.second;
    int y = dt.year, m = dt.month;
    if (m < 3) { m += 12; y--; }
    int k = y % 100, j = y / 100;
    int h = (dt.day + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;   /* 0 = Saturday */
    t->weekday = (h + 6) % 7;
}

static unsigned int a_time_utc(void) { return rtc_now_utc(); }
static int a_utc_offset(unsigned int utc) { return tz_offset_at(tz_current(), utc ? utc : rtc_now_utc()); }
static int a_language(char* out, int cap) { if (!out || cap < 3) return -1; kstrlcpy(out, i18n_lang(), (size_t)cap); return 0; }

static unsigned int a_random(void) { return random_u32(); }

/* ── windows ───────────────────────────────────────────────────────── */

static int a_win_open(const char* title, int w, int h) {
    app_proc_t* p = cur();
    if (!p || !gui_is_enabled()) return -1;
    return appwin_open_app(p->id, title, w, h);
}
static unsigned int* a_win_pixels(int win) { app_proc_t* p = cur(); return p ? appwin_pixels(win, p->id) : NULL; }
static void a_win_update(int win) {
    app_proc_t* p = cur();
    if (!p) return;
    appwin_update(win, p->id);
    breathe(p, 0);
}
static int a_win_event(int win, banana_event_t* ev) {
    app_proc_t* p = cur();
    if (!p || !ev) return 0;
    breathe(p, 0);
    return appwin_event(win, p->id, ev);
}
static void a_win_close(int win) { app_proc_t* p = cur(); if (p) appwin_close(win, p->id); }
static void a_win_set_title(int win, const char* t) { app_proc_t* p = cur(); if (p) appwin_set_title(win, p->id, t); }
static void a_win_size(int win, int* w, int* h) { app_proc_t* p = cur(); appwin_size(win, p ? p->id : -1, w, h); }
static void a_win_media_keys(int win) { app_proc_t* p = cur(); if (p) appwin_set_media(win, p->id); }
static int a_win_capture_mouse(int win,int capture) { app_proc_t* p=cur();return p?appwin_capture_mouse(win,p->id,capture):-1; }
static int a_win_mouse_captured(int win) { app_proc_t* p=cur();return p?appwin_mouse_captured(win,p->id):-1; }
static void a_win_wheel(int win) { app_proc_t* p = cur(); if (p) appwin_set_wheel(win, p->id); }
static void a_win_set_resizable(int win, int mw, int mh) { app_proc_t* p = cur(); if (p) appwin_set_resizable(win, p->id, mw, mh); }

/* web views (webview.c): each belongs to the app that opened it */
static int  owner_id(void) { app_proc_t* p = cur(); return p ? p->id : -1; }
static int  a_web_open(int w, int h) { int o = owner_id(); return o < 0 ? -1 : webview_open(o, w, h); }
static void a_web_close(int v) { webview_close(owner_id(), v); }
static int  a_web_load(int v, const char* url) { return webview_load(owner_id(), v, url); }
static int  a_web_load_html(int v, const char* h, const char* base) { return webview_load_html(owner_id(), v, h, base); }
static void a_web_resize(int v, int w, int h) { webview_resize(owner_id(), v, w, h); }
static int  a_web_poll(int v) { return webview_poll(owner_id(), v); }
static void a_web_draw(int v, unsigned int* px, int stride, int x, int y, int w, int h) {
    if (!px || w <= 0 || h <= 0 || stride < w || !uok(px, (size_t)stride * (size_t)h * 4)) return;
    webview_draw(owner_id(), v, px, stride, x, y, w, h);
}
static void a_web_event(int v, const banana_event_t* ev) { webview_event(owner_id(), v, ev); }
static void a_web_scroll(int v, int dy) { webview_scroll(owner_id(), v, dy); }
static void a_web_go(int v, int d) { webview_go(owner_id(), v, d); }
static int  a_web_info(int v, char* t, int tc, char* u, int uc) { return webview_info(owner_id(), v, t, tc, u, uc); }
static int  a_web_eval(int v, const char* js, char* out, int cap) { return webview_eval(owner_id(), v, js, out, cap); }
static int  a_web_message(int v, char* out, int cap) { return webview_message(owner_id(), v, out, cap); }
static int  a_web_post(int v, const char* m) { return webview_post(owner_id(), v, m); }

/* ── pictures ── */
static unsigned int* a_image_load(const char* path, int* w, int* h, char* err, int ecap) {
    char e[96];
    if (!err || ecap <= 0) { err = e; ecap = sizeof(e); }
    err[0] = 0;
    int idx = path ? fs_find_file(path) : -1;
    fs_file_t* f = idx >= 0 ? fs_get_file(idx) : NULL;
    if (!f) { ksnprintf(err, (uint32_t)ecap, "no such file"); return NULL; }
    image_t img;
    fs_pin(idx);
    int bad = image_decode((const uint8_t*)f->content, f->size, &img, err, (uint32_t)ecap) != 0;
    fs_unpin(idx);
    if (bad) return NULL;
    unsigned int* px = (unsigned int*)a_malloc((unsigned long)img.w * (unsigned long)img.h * 4ul);
    if (!px) { image_free(&img); ksnprintf(err, (uint32_t)ecap, "out of memory"); return NULL; }
    for (int i = 0; i < img.w * img.h; i++)
        px[i] = (unsigned int)img.rgb[i * 3] << 16 | (unsigned int)img.rgb[i * 3 + 1] << 8 | img.rgb[i * 3 + 2];
    if (w) *w = img.w;
    if (h) *h = img.h;
    image_free(&img);
    return px;
}

static int a_set_wallpaper(const char* path, int mode, char* err, int ecap) {
    char e[96];
    if (!err || ecap <= 0) { err = e; ecap = sizeof(e); }
    if (!path) return -1;
    if (mode < 0 || mode >= IMAGE_MODE_COUNT) mode = IMAGE_FILL;
    char abs[FS_PATH_LEN];
    int idx = fs_find_file(path);
    if (idx < 0) { ksnprintf(err, (uint32_t)ecap, "no such file"); return -1; }
    fs_file_path(idx, abs, sizeof(abs));
    return wallpaper_set_file(abs, (image_mode_t)mode, err, (uint32_t)ecap);
}

/* the system fonts */
static int clamp_size(int s) { return s < 4 ? 4 : s > 200 ? 200 : s; }
static int a_font_draw(unsigned int* px, int stride, int w, int h, int x, int y, int font, int size, const char* text, unsigned int color) {
    if (!px || !text || w <= 0 || h <= 0 || stride < w) return x;
    if (!uok(px, (size_t)stride * (size_t)h * 4)) return x;
    size = clamp_size(size);
    int asc;
    font_metrics(font, size, &asc, NULL, NULL);
    return font_draw(px, stride, 0, 0, w, h, font, size, x, y + asc, text, (uint32_t)strlen(text), color, 0);
}
static int a_font_width(int font, int size, const char* text) {
    return text ? font_text_width(font, clamp_size(size), text, (uint32_t)strlen(text)) : 0;
}
static unsigned int a_audio_queued_ms(void) { return audio_queued_ms(); }
static void a_audio_stop(void) { audio_stop(); }
static int a_audio_volume(int percent) {
    if (percent >= 0) audio_set_volume(percent > 100 ? 100 : percent);
    return audio_get_volume();
}

static void a_font_metrics(int font, int size, int* ascent, int* descent, int* line_h) {
    font_metrics(font, clamp_size(size), ascent, descent, line_h);
}

static void a_draw_text(unsigned int* px, int stride, int w, int h, int x, int y,
                        const char* s, unsigned int fg, unsigned int bg) {
    if (!px || !s || w <= 0 || h <= 0 || stride < w || !uok(px, (size_t)stride * (size_t)h * 4)) return;
    for (; *s; s++, x += 8) {
        uint8_t c = (uint8_t)*s;
        if (c >= 128) c = '?';
        const uint8_t* g = font8x8_basic[c];
        for (int gy = 0; gy < 8; gy++) {
            int yy = y + gy;
            if (yy < 0 || yy >= h) continue;
            for (int gx = 0; gx < 8; gx++) {
                int xx = x + gx;
                if (xx < 0 || xx >= w) continue;
                if (g[gy] & (1u << gx)) px[yy * stride + xx] = fg;
                else if (bg != BANANA_TRANSPARENT) px[yy * stride + xx] = bg;
            }
        }
    }
}

/* ── network ───────────────────────────────────────────────────────── */

typedef struct { char* buf; uint32_t n, cap; int oom; } hbody_t;

static int h_body(void* ctx, const uint8_t* d, uint32_t n) {
    hbody_t* b = (hbody_t*)ctx;
    if (b->n + n + 1 > (16u << 20)) { b->oom = 1; return -1; }
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = (b->n + n + 1) * 2;
        char* nb = (char*)a_realloc(b->buf, cap);
        if (!nb) { b->oom = 1; return -1; }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->n, d, n);
    b->n += n;
    b->buf[b->n] = 0;
    return 0;
}

static int a_http_request(const char* method, const char* url, const char* body, unsigned long blen, const char* btype,
                          char** data, unsigned long* len, char* ctype, int ccap, char* err, int ecap) {
    char e[128];
    hbody_t b = { 0, 0, 0, 0 };
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = method && method[0] ? method : "GET";
    if (body) {
        req.body = body;
        req.body_len = (uint32_t)blen;
        req.content_type = btype && btype[0] ? btype : "application/x-www-form-urlencoded";
    }
    req.follow_redirects = 1;
    req.max_redirects = 10;
    req.timeout_ms = 15000;
    req.user_agent = "BananaOS-App/1.0";
    req.ctx = &b;
    req.on_body = h_body;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) return -1;
    int rc = http_fetch(url, &req, resp, e, sizeof(e));
    if (rc != NET_OK || b.oom) {
        if (err && ecap > 0) kstrlcpy(err, b.oom ? "too big (16 MiB max)" : e, (size_t)ecap);
        if (b.buf) a_free(b.buf);
        kfree(resp);
        return -1;
    }
    if (ctype && ccap > 0) kstrlcpy(ctype, resp->content_type, (size_t)ccap);
    if (resp->status >= 400) {
        if (err && ecap > 0) ksnprintf(err, (size_t)ecap, "HTTP %d %s", resp->status, resp->reason);
        kfree(resp);
        if (b.buf) a_free(b.buf);
        return -1;
    }
    kfree(resp);
    if (!b.buf) { b.buf = (char*)a_malloc(1); if (!b.buf) return -1; b.buf[0] = 0; }
    *data = b.buf;
    if (len) *len = b.n;
    return 0;
}

/* any request, any answer (version 10) */
static int a_http_fetch(const banana_http_req_t* rq, banana_http_resp_t* out) {
    if (!rq || !out || !uok(rq, sizeof(*rq)) || !uok(out, sizeof(*out))) return -1;
    /* (what the request points at must be the app's too) */
    if (!rq->url || !ustr(rq->url) || (rq->method && !ustr(rq->method)) || (rq->headers && !ustr(rq->headers)) ||
        (rq->content_type && !ustr(rq->content_type)) || (rq->body && !uok(rq->body, rq->body_len ? rq->body_len : 1)))
        return -1;
    memset(out, 0, sizeof(*out));
    hbody_t b = { 0, 0, 0, 0 };
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = rq->method && rq->method[0] ? rq->method : "GET";
    if (rq->body) {
        req.body = rq->body;
        req.body_len = (uint32_t)rq->body_len;
        req.content_type = rq->content_type && rq->content_type[0] ? rq->content_type : "application/json";
    }
    req.extra_headers = rq->headers;
    req.follow_redirects = 1;
    req.max_redirects = 10;
    req.timeout_ms = rq->timeout_ms > 0 ? (uint32_t)rq->timeout_ms : 20000;
    req.user_agent = "BananaOS-App/1.0";
    req.ctx = &b;
    req.on_body = h_body;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) { kstrlcpy(out->err, "out of memory", sizeof(out->err)); return -1; }
    char e[128];
    int rc = http_fetch(rq->url, &req, resp, e, sizeof(e));
    if (rc != NET_OK || b.oom) {
        kstrlcpy(out->err, b.oom ? "too big (16 MiB max)" : e, sizeof(out->err));
        if (b.buf) a_free(b.buf);
        kfree(resp);
        return -1;
    }
    out->status = resp->status;
    kstrlcpy(out->content_type, resp->content_type, sizeof(out->content_type));
    kfree(resp);
    if (!b.buf) { b.buf = (char*)a_malloc(1); if (b.buf) b.buf[0] = 0; }
    out->data = b.buf;
    out->len = b.n;
    return 0;
}

static int a_key_mods(void) { return keyboard_mods(); }

static int a_pkg_install(const char* path, char* msg, int mcap) {
    char m[160];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!path) return -1;
    return pkg_install(path, msg, mcap) == 0 ? 0 : -1;
}

/* ── version 11: the package manager ── */
static int a_repo_update(char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    return repo_update(NULL, NULL, msg, mcap);
}

static void to_api(banana_pkg_t* o, const repo_pkg_t* p, const pkg_info_t* inst) {
    memset(o, 0, sizeof(*o));
    const char* name = p ? p->name : inst->name;
    kstrlcpy(o->name, name, sizeof(o->name));
    kstrlcpy(o->title, p ? p->title : inst->title, sizeof(o->title));
    kstrlcpy(o->version, p ? p->version : inst->version, sizeof(o->version));
    kstrlcpy(o->type, p ? p->type : inst->type, sizeof(o->type));
    kstrlcpy(o->category, p && p->category[0] ? p->category : inst ? inst->category : "", sizeof(o->category));
    kstrlcpy(o->description, p ? p->description : inst->description, sizeof(o->description));
    kstrlcpy(o->author, p ? p->author : inst->author, sizeof(o->author));
    kstrlcpy(o->depends, p ? p->depends : inst->depends, sizeof(o->depends));
    if (p) { o->size = p->size; o->flags |= BANANA_PKG_AVAILABLE; }
    if (inst) {
        kstrlcpy(o->installed, inst->version[0] ? inst->version : "0", sizeof(o->installed));
        o->flags |= BANANA_PKG_INSTALLED;
        if (repo_is_auto(name)) o->flags |= BANANA_PKG_AUTO;
        if (p && repo_vercmp(p->version, inst->version) > 0) o->flags |= BANANA_PKG_UPGRADABLE;
    }
}

static int a_repo_list(banana_pkg_t* out, int max) {
    if (!out || max <= 0 || !uok(out, (size_t)max * sizeof(banana_pkg_t))) return 0;
    static pkg_info_t inst[64];
    int ni = pkg_list(inst, 64);
    if (ni > 64) ni = 64;
    int n = 0, total = repo_count();
    for (int i = 0; i < total; i++) {
        const repo_pkg_t* p = repo_at(i);
        if (!p || p != repo_candidate(p->name)) continue;
        const pkg_info_t* in = NULL;
        for (int k = 0; k < ni; k++) if (!strcmp(inst[k].name, p->name)) in = &inst[k];
        if (n < max) to_api(&out[n], p, in);
        n++;
    }
    for (int k = 0; k < ni; k++) {                       /* installed, in no repository */
        if (repo_candidate(inst[k].name)) continue;
        if (n < max) to_api(&out[n], NULL, &inst[k]);
        n++;
    }
    return n < max ? n : max;
}

static int a_repo_install(const char* name, char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!name) return -1;
    return repo_install(name, 0, NULL, NULL, msg, mcap);
}

static int a_repo_remove(const char* name, char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!name) return -1;
    return repo_remove(name, msg, mcap);
}

static int a_repo_status(char* msg, int mcap) { return repo_status(msg, mcap); }

static int a_repo_icon(const char* name, char* path, int pcap) {
    if (!name || !path || pcap <= 0) return -1;
    return repo_icon(name, path, pcap);
}

static int a_repo_upgrade(char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    return repo_upgrade(NULL, NULL, msg, mcap);
}

static int a_repo_sources(banana_repo_source_t* out, int max) {
    static repo_source_t src[16];
    int n = repo_sources(src, 16);
    if (!out || max <= 0) return n;
    if (!uok(out, (size_t)max * sizeof(banana_repo_source_t))) return -1;
    for (int i = 0; i < n && i < max; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        kstrlcpy(out[i].url, src[i].url, sizeof(out[i].url));
        out[i].has_key = src[i].has_key;
        out[i].trusted = src[i].trusted;
    }
    return n;
}

static int a_repo_add_source(const char* url, const char* key_hex, int trusted, char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!url) return -1;
    if (key_hex && !key_hex[0]) key_hex = NULL;
    if (!key_hex && !trusted && strncmp(url, "file://", 7)) {
        kstrlcpy(msg, "give the server's key, or choose to trust it without one", (size_t)mcap);
        return -1;
    }
    return repo_add_source(url, key_hex, trusted, msg, mcap);
}

static int a_repo_remove_source(const char* url, char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!url || !url[0] || (url[0] >= '0' && url[0] <= '9')) return -1;   /* by URL only, not by number */
    return repo_remove_source(url, msg, mcap);
}

static int a_win_dropped(int win, int index, char* path, int cap) {
    app_proc_t* p = cur();
    return appwin_dropped(win, p ? p->id : -1, index, path, cap);
}

static int a_gamepad(int index, banana_gamepad_t* out) {
    gamepad_t g;
    int r = gamepad_get(index, &g);
    if (out) {
        memset(out, 0, sizeof(*out));
        out->connected = r;
        kstrlcpy(out->name, g.name, sizeof(out->name));
        out->lx = g.lx; out->ly = g.ly; out->rx = g.rx; out->ry = g.ry;
        out->lt = g.lt; out->rt = g.rt;
        out->buttons = g.buttons;
    }
    return r;
}

/* a key held now, if the app has the keyboard (its terminal or its window in front) */
static int a_key_down(int key) {
    app_proc_t* p = cur();
    if (!p || !(focused(p) || appwin_focused_owner() == p->id)) return 0;
    switch (key) {                      /* the arrows etc.: the E0 keys, or the keypad's */
    case BANANA_KEY_UP:     return keyboard_held(0xC8) || keyboard_held(0x48);
    case BANANA_KEY_DOWN:   return keyboard_held(0xD0) || keyboard_held(0x50);
    case BANANA_KEY_LEFT:   return keyboard_held(0xCB) || keyboard_held(0x4B);
    case BANANA_KEY_RIGHT:  return keyboard_held(0xCD) || keyboard_held(0x4D);
    case BANANA_KEY_HOME:   return keyboard_held(0xC7) || keyboard_held(0x47);
    case BANANA_KEY_END:    return keyboard_held(0xCF) || keyboard_held(0x4F);
    case BANANA_KEY_PGUP:   return keyboard_held(0xC9) || keyboard_held(0x49);
    case BANANA_KEY_PGDN:   return keyboard_held(0xD1) || keyboard_held(0x51);
    case BANANA_KEY_DELETE: return keyboard_held(0xD3) || keyboard_held(0x53);
    case BANANA_KEY_SHIFT:  return keyboard_held(0x2A) || keyboard_held(0x36);
    case BANANA_KEY_CTRL:   return keyboard_held(0x1D) || keyboard_held(0x9D);
    case BANANA_KEY_ALT:    return keyboard_held(0x38) || keyboard_held(0xB8);
    }
    if (key >= BANANA_KEY_F1 && key <= BANANA_KEY_F12) {
        int i = key - BANANA_KEY_F1;
        return keyboard_held(i < 10 ? 0x3B + i : 0x57 + (i - 10));
    }
    return keyboard_held_char(key);
}

static int a_app_run(const char* name, int argc, char** argv, char* err, int ecap) {
    char e[128];
    if (!err || ecap <= 0) { err = e; ecap = sizeof(e); }
    if (!name || argc < 0 || argc > 256 || (argc && !uok(argv, (size_t)argc * sizeof(char*)))) return -1;
    for (int i = 0; i < argc; i++) if (!argv[i] || !ustr(argv[i])) return -1;
    return pkg_run(name, argc, argv, 1, err, ecap) < 0 ? -1 : 0;
}

static int a_http_get(const char* url, char** data, unsigned long* len, char* ctype, int ccap, char* err, int ecap) {
    return a_http_request("GET", url, NULL, 0, NULL, data, len, ctype, ccap, err, ecap);
}

/* ── sound / clipboard ─────────────────────────────────────────────── */

static int a_audio_play(const void* pcm, unsigned long bytes, int rate, int ch, int bits) {
    return audio_play(pcm, (uint32_t)bytes, rate, ch, bits);
}
static void a_audio_beep(int hz, int ms) { audio_beep(hz, ms); }
static int a_audio_busy(void) { return audio_busy(); }
static void a_clip_set(const char* t, unsigned long n) { if (t) clipboard_set(t, (uint32_t)n); }
static const char* a_clip_get(unsigned long* n) {
    app_proc_t* p = cur();
    uint32_t l;
    const char* s = clipboard_get(&l);
    if (n) *n = s ? l : 0;
    if (!s || !p) return NULL;
    /* the app gets its own copy (the kernel's is not its to read), kept until the next call */
    char* c = (char*)umem_realloc(p->mem, p->clip, (size_t)l + 1);
    if (!c) return NULL;
    memcpy(c, s, l);
    c[l] = 0;
    p->clip = c;
    return c;
}

/* ── threads ───────────────────────────────────────────────────────── */

#ifdef __x86_64__
int app_enter_user(uintptr_t ip, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t user_sp, uintptr_t* saved_sp);

/* the kernel stack of an app thread's system calls, a guard page under it:
 * running past its end faults (the app stops) instead of overwriting the heap */
static uint8_t* kstack_new(void) {
    uint8_t* m = (uint8_t*)kmalloc_aligned(APP_KSTACK + 4096, 4096);
    if (m) paging_guard((uintptr_t)m, 4096, 1);
    return m;
}

static void kstack_free(uint8_t* m) {
    if (!m) return;
    paging_guard((uintptr_t)m, 4096, 0);
    kfree(m);
}

static uintptr_t kstack_top(const uint8_t* m) { return (uintptr_t)m + 4096 + APP_KSTACK; }

/* runs fn(a0, a1, a2) in ring 3 on the stack [stack, stack + size) until
 * it returns or the app ends: its exit code */
static int enter_user(app_proc_t* p, uintptr_t fn, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                      uint8_t* stack, size_t size, uint8_t* kstack, uintptr_t* saved_sp) {
    /* where fn returns to: a system call that ends the thread (at 8 mod 16,
     * as after a call) */
    uintptr_t usp = (((uintptr_t)stack + size) & ~(uintptr_t)15) - 8;
    task_set_user(uspace_cr3(p->us), uspace_tag(p->us), kstack_top(kstack));
    *(uintptr_t*)usp = p->ret_stub;         /* (a stack it mapped: its tables are loaded now) */
    int rc = app_enter_user(fn, a0, a1, a2, usp, saved_sp);
    task_set_user(0, NULL, 0);              /* the kernel's tables: the app's may go now */
    task_set_fs(0);
    return rc;
}
#else
static int thread_tramp(void* arg) {
    athread_t* t = (athread_t*)arg;
    return t->fn(t->arg);
}
#endif

/* a thread's stacks back to the app / the kernel */
static void thread_free(app_proc_t* p, athread_t* t) {
    if (!t->own_stack) umem_free_pages(p->mem, t->stack, APP_GUARD);
#ifdef __x86_64__
    kstack_free(t->kstack);
#endif
    t->stack = NULL;
    t->kstack = NULL;
}

static void thread_entry(void) {
    app_proc_t* p = cur();
    athread_t* t = cur_thread(p);
    if (!p || !t) return;
    if (p->has_term) terminal_vt_set_active(p->vt);
    else task_set_background();
    t->running = 1;
#ifdef __x86_64__
    t->ret = enter_user(p, (uintptr_t)t->fn, (uintptr_t)t->arg, 0, 0, t->stack, t->stack_size, t->kstack, &t->saved_sp);
#else
    t->ret = app_enter(thread_tramp, t, t->stack + t->stack_size, &t->saved_sp);
#endif
    t->running = 0;
    /* The task ends now and its slot (and pid) goes to the next task
     * created - another app's thread, a kernel task. The record stays
     * for banana_thread_join, but must no longer answer to that pid:
     * it did, and a new thread of another app was taken for this one
     * (cur() found this app first) and ran this thread's code again on
     * its old stack - both apps crashed, and then the kernel. */
    pid_clear(t->pid, p->id);
    t->pid = -1;
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    t->done = 1;
    int gone = t->detached;                 /* nobody will join it: its record goes now */
    __asm__ volatile("push %0; popf" :: "r"(f) : "memory", "cc");
    if (gone) { thread_free(p, t); t->used = 0; }
}

/* a new thread running fn(arg): on stack [stack, stack + size) if the app
 * gives one (its memory, kept by it), else on size bytes (0: APP_STACK)
 * of new memory with guard pages under it; its id, or -1 */
static int thread_new(int (*fn)(void*), void* arg, void* stack, unsigned long size) {
    app_proc_t* p = cur();
    if (!p || !fn || p->exit_req || !uok((const void*)fn, 1)) return -1;
    if (stack ? (size < 16384 || !uok(stack, size)) : size > (256ul << 20)) return -1;
    athread_t* t = NULL;
    int id = -1;
    for (int i = 0; i < APP_THREADS; i++) if (!p->threads[i].used) { t = &p->threads[i]; id = i; break; }
    if (!t) return -1;
    memset(t, 0, sizeof(*t));
    t->sig.blocked = sig_state(p)->blocked;     /* (a new thread starts with its creator's mask) */
    if (stack) {
        t->stack = (uint8_t*)stack;
        t->stack_size = size;
        t->own_stack = 1;
    } else {
        t->stack_size = size ? (size + 4095) & ~4095ul : APP_STACK;
        t->stack = (uint8_t*)umem_alloc_pages(p->mem, t->stack_size, APP_GUARD);
    }
#ifdef __x86_64__
    t->kstack = kstack_new();
    if (!t->kstack) { thread_free(p, t); return -1; }
#endif
    if (!t->stack) { thread_free(p, t); return -1; }
    t->fn = fn;
    t->arg = arg;
    t->pid = -1;
    t->used = 1;
    char name[24];
    char base[15];                          /* (ksnprintf has no %.14s) */
    kstrlcpy(base, p->name, sizeof(base));
    ksnprintf(name, sizeof(name), "%s/t%d", base, id + 1);
    int pid = task_create(name, thread_entry);
    if (pid < 0) { thread_free(p, t); t->used = 0; return -1; }
    /* (no record of an ended thread may still answer to this pid) */
    for (int i = 0; i < APP_MAX; i++) {
        if (!g_procs[i].used) continue;
        for (int k = 0; k < APP_THREADS; k++)
            if (&g_procs[i].threads[k] != t && g_procs[i].threads[k].used && g_procs[i].threads[k].pid == pid)
                g_procs[i].threads[k].pid = -1;
    }
    t->pid = pid;                           /* it runs once we yield (no preemption in here) */
    pid_set(pid, p->id, id);
    return id + 1;
}

static int a_thread_create(int (*fn)(void*), void* arg) { return thread_new(fn, arg, NULL, 0); }

static int a_thread_start(int (*fn)(void*), void* arg, void* stack, unsigned long size) {
    return thread_new(fn, arg, stack, size);
}

/* nobody will thread_join it: its record (and stack) go when it ends */
static int a_thread_detach(int id) {
    app_proc_t* p = cur();
    if (!p || id < 1 || id > APP_THREADS) return -1;
    athread_t* t = &p->threads[id - 1];
    if (!t->used || t->detached) return -1;
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    t->detached = 1;
    int done = t->done;
    __asm__ volatile("push %0; popf" :: "r"(f) : "memory", "cc");
    if (done) { thread_free(p, t); t->used = 0; }
    return 0;
}

/* the calling thread's thread pointer: FS base (64-bit TLS, <pthread.h>) */
static int a_tls_set(void* tp) {
#ifdef __x86_64__
    if (!cur()) return -1;
    task_set_fs((uintptr_t)tp);
    return 0;
#else
    (void)tp;
    return -1;
#endif
}

/* ── signals (version 19): the calls ──────────────────────────────────── */

static sigstate_t* sig_state(app_proc_t* p) {
    athread_t* t = cur_thread(p);
    return t ? &t->sig : &p->msig;
}

static int a_sig_entry(void (*entry)(int, void*, void*)) {
    app_proc_t* p = cur();
    if (!p || (entry && !uok((const void*)entry, 1))) return -1;
    p->sig_entry = (uintptr_t)entry;
    return 0;
}

static int a_sig_action(int sig, int disp, int onstack) {
    app_proc_t* p = cur();
    if (!p || sig < 1 || sig > 64 || disp < 0 || disp > 2) return -1;
    if ((sig == 9 || sig == 19) && disp) return -1;             /* SIGKILL, SIGSTOP: never */
    p->sig_disp[sig] = (uint8_t)disp;
    if (onstack) p->sig_onstack |= 1ull << (sig - 1);
    else p->sig_onstack &= ~(1ull << (sig - 1));
    return 0;
}

static int a_sig_mask(int how, const unsigned long long* set, unsigned long long* old) {
    app_proc_t* p = cur();
    if (!p || (set && !uok(set, 8)) || (old && !uok(old, 8))) return -1;
    sigstate_t* st = sig_state(p);
    if (old) *old = st->blocked;
    if (!set) return 0;
    uint64_t s = *set & ~((1ull << 8) | (1ull << 18));         /* (SIGKILL, SIGSTOP) */
    if (how == 0) st->blocked |= s;
    else if (how == 1) st->blocked &= ~s;
    else if (how == 2) st->blocked = s;
    else return -1;
    return 0;
}

/* to thread id (thread_id()'s numbering), or the app (-1); it is
 * delivered when that thread next goes back to its code */
static int a_sig_send(int thread, int sig) {
    app_proc_t* p = cur();
    if (!p || sig < 0 || sig > 64) return -1;
    if (thread < -1 || thread > APP_THREADS) return -1;
    sigstate_t* st = thread == -1 ? NULL : thread == 0 ? &p->msig : &p->threads[thread - 1].sig;
    if (thread > 0 && (!p->threads[thread - 1].used || p->threads[thread - 1].done)) return -1;
    if (!sig) return 0;                                          /* (only: does it exist?) */
#ifdef __x86_64__
    if (st) st->pending |= 1ull << (sig - 1);
    else p->sig_pending |= 1ull << (sig - 1);
    /* a thread sleeping in wait() looks at its signals (EINTR-like wake) */
    if (thread > 0 && p->threads[thread - 1].pid >= 0) task_wake(p->threads[thread - 1].pid);
    else if (thread == 0 || thread == -1) task_wake(p->pid);
    return 0;
#else
    (void)st;
    return -1;                                                   /* (32-bit: raise() only, in the SDK) */
#endif
}

static int a_sig_altstack(const void* ss, void* old) {
    app_proc_t* p = cur();
    if (!p || (ss && !uok(ss, sizeof(banana_stack_t))) || (old && !uok(old, sizeof(banana_stack_t)))) return -1;
    sigstate_t* st = sig_state(p);
    if (old) {
        banana_stack_t* o = (banana_stack_t*)old;
        o->ss_sp = (void*)st->alt_sp;
        o->ss_size = st->alt_size;
        o->ss_flags = st->alt_size ? 0 : 2;                      /* SS_DISABLE */
    }
    if (ss) {
        const banana_stack_t* n = (const banana_stack_t*)ss;
        if (n->ss_flags & 2) { st->alt_sp = 0; st->alt_size = 0; return 0; }
        if (n->ss_size < 2048 || !uok(n->ss_sp, n->ss_size)) return -1;
        st->alt_sp = (uintptr_t)n->ss_sp;
        st->alt_size = n->ss_size;
    }
    return 0;
}

static unsigned long long a_sig_pending(void) {
    app_proc_t* p = cur();
    if (!p) return 0;
    return sig_state(p)->pending | p->sig_pending;
}

/* 1 if the calling thread has a signal to take now (a wait should end) */
static int sig_waiting(app_proc_t* p) {
#ifdef __x86_64__
    sigstate_t* st = sig_state(p);
    return ((st->pending | p->sig_pending) & ~st->blocked) != 0;
#else
    (void)p;
    return 0;                                   /* (32-bit: nothing is delivered) */
#endif
}

/* the computer's memory for apps, in bytes: what there is and what is free
 * (sysconf(_SC_PHYS_PAGES) - a Java heap is sized from it) */
static void a_mem_info(unsigned long long* total, unsigned long long* avail) {
    meminfo_t mi;
    meminfo_get(&mi);
    uint64_t ht = hmem_total_bytes(), hu = hmem_used_bytes();
    if (total) *total = (uint64_t)mi.total_kb * 1024 + ht;
    if (avail) *avail = (uint64_t)mi.avail_kb * 1024 + (ht > hu ? ht - hu : 0);
}

static int a_app_id(void) { app_proc_t* p = cur(); return p ? p->seq : -1; }

static int a_thread_join(int id) {
    app_proc_t* p = cur();
    if (!p || id < 1 || id > APP_THREADS) return -1;
    athread_t* t = &p->threads[id - 1];
    if (!t->used || t == cur_thread(p)) return -1;
    while (!t->done) {
        breathe(p, 1);
        task_sleep_ms(1);
    }
    int r = t->ret;
    thread_free(p, t);
    t->used = 0;
    return r;
}

static int a_thread_id(void) {
    app_proc_t* p = cur();
    athread_t* t = cur_thread(p);
    return t ? (int)(t - p->threads) + 1 : 0;
}

static void a_thread_exit(int ret) {
    app_proc_t* p = cur();
    athread_t* t = cur_thread(p);
    if (t) leave_thread(p, t, ret);
    if (p) leave(p, ret);                   /* the main thread: the app ends */
    for (;;) task_sleep_ms(1000);
}

static int a_gpu_call(unsigned op, const void* request, unsigned request_size, void* data, unsigned size) {
    app_proc_t* p = cur();
    return p ? gpu_virgl_call(p->id + 1, op, request, request_size, data, size) : -1;
}
static int a_cpu_count(void) { return cpu_count(); }

/* wait / wake: a thread sleeps while *addr == expected, until another one
 * changes it and calls wake (or the timeout passes) - what the SDK builds
 * condition variables and semaphores on */
#define WAITERS 256
static struct { volatile int* addr; int pid; } g_waiters[WAITERS];

static int waiter_add(volatile int* addr) {
    for (int i = 0; i < WAITERS; i++)
        if (!g_waiters[i].addr) { g_waiters[i].addr = addr; g_waiters[i].pid = task_current_pid(); return i; }
    return -1;
}

static int a_wait(volatile int* addr, int expected, int timeout_ms) {
    app_proc_t* p = cur();
    if (!p || !addr) return -1;
    uint32_t end = timer_ms() + (uint32_t)(timeout_ms > 0 ? timeout_ms : 0);
    breathe(p, 1);                          /* paints, notices Ctrl+C / End task (may not return) */
    for (;;) {
        /* woken: straight back to the app (breathing first would yield) */
        if (*addr != expected) return 0;
        if (timeout_ms >= 0 && (int32_t)(timer_ms() - end) >= 0) return 1;
        breathe(p, 0);                      /* (every 20 ms) */
        if (sig_waiting(p)) return 0;       /* a signal for it: it goes to the handler first */
        if (*addr != expected) return 0;
        /* registered only while asleep: if the app is stopped in breathe()
         * no stale entry is left behind; a wake in between costs <= 10 ms */
        int slot = waiter_add(addr);
        if (*addr == expected) task_sleep_ms(slot >= 0 ? 10 : 1);
        if (slot >= 0) g_waiters[slot].addr = NULL;
    }
}

static int a_wake(volatile int* addr, int count) {
    int n = 0;
    for (int i = 0; i < WAITERS; i++) {
        if (g_waiters[i].addr != addr) continue;
        task_wake(g_waiters[i].pid);
        if (++n >= count && count > 0) break;
    }
    return n;
}

/* kernel/idt.c, on every hardware interrupt: if it came while an app's
 * own code was running (not inside the kernel), switching to another task
 * is safe - so a busy app or thread cannot freeze the desktop, and the
 * threads of an app share the CPU. An app asked to stop is stopped here. */
#ifdef __x86_64__
static void sig_preempt(app_proc_t* p, void* regs);
#endif

void app_preempt(uintptr_t ip, int from_user, void* regs) {
    app_proc_t* p = cur();
#ifdef __x86_64__
    /* (anything in ring 3 is the app's: its code, and code it made - tcc, OpenCL) */
    (void)ip;
    if (!p || !from_user) return;
#else
    (void)from_user;
    if (!p || ip < p->code_lo || ip >= p->code_hi) return;
#endif
    athread_t* t = cur_thread(p);
    if (t ? !t->running : !p->running) return;
    __asm__ volatile("sti");
    if (p->exit_req || p->kill_req) leave(p, p->exit_req ? p->exit_req_code : 137);
    /* what an app call does: paint the desktop (the app may run in the task
     * that draws it), notice Ctrl+C / End task, let the other tasks run */
    breathe(p, 0);
    task_maybe_yield();                     /* slice over, or a woken task waits (task_should_yield) */
#ifdef __x86_64__
    sig_preempt(p, regs);                   /* signals sent to it: to their handlers */
#else
    (void)regs;
#endif
    if (p->has_term) terminal_vt_set_active(p->vt);
    /* a free processor core takes the app code from here (kernel/task.h):
     * nothing of the kernel may run after this - it may be on that core now */
    task_offload();
    __asm__ volatile("cli");
}

uintptr_t app_fault_stack(int pid) {
    for (int i = 0; i < APP_MAX; i++) {
        app_proc_t* p = &g_procs[i];
        if (!p->used) continue;
        if (p->pid == pid && p->running) return p->saved_sp;
        for (int t = 0; t < APP_THREADS; t++)
            if (p->threads[t].used && p->threads[t].pid == pid && p->threads[t].running) return p->threads[t].saved_sp;
    }
    return 0;
}

static void a_exit(int code) {
    app_proc_t* p = cur();
    if (p) leave(p, code);
    for (;;) task_sleep_ms(1000);    /* not an app: cannot happen */
}

#ifdef __x86_64__
/* ── system calls (64-bit) ─────────────────────────────────────────────
 * An app runs in ring 3 and reaches the kernel only through int 0x80:
 * every function entry of the table it gets is a stub "mov eax, slot;
 * int 0x80; ret" in its own memory (make_uapi), so apps built for the
 * table of function pointers work unchanged. Before a call runs, every
 * pointer it was given is checked to be the app's memory (the kernel
 * would otherwise read or write anywhere on its behalf); a call that
 * fails the check stops the app, like a bad memory access would. */

#define API_SLOTS  (sizeof(banana_api_t) / sizeof(void*))
#define SYS_RETURN API_SLOTS            /* the entry or a thread's function returned (ret_stub) */
#define SYS_ARGS   12                   /* the most a call takes (http_request: 11) */

/* What each argument is, one letter per argument:
 *   i  a number (or a pointer the call checks itself: x)
 *   s  a string            b / B  a buffer, its size the next argument (int / unsigned long)
 *   I  int*   L  a pointer or unsigned long out   o  a struct of the slot's osize */
typedef struct { const char* args; uint16_t osize; } sysspec_t;
static sysspec_t g_spec[API_SLOTS];

#define SPEC(f, a, o) (g_spec[__builtin_offsetof(banana_api_t, f) / sizeof(void*)] = (sysspec_t){ a, o })

/* Calls that may run on whichever core the app thread makes them on - they
 * touch only the app's own state and what has locks of its own (its
 * memory: kernel/umem.c, the heap) and never wait or switch tasks. Every
 * other call first takes the thread back to the boot core, where the rest
 * of the kernel runs. wait has a quick case here and goes home only
 * when it really has to sleep (LOCAL_WAIT). */
enum { LOCAL_NO = 0, LOCAL_YES, LOCAL_WAIT };
static uint8_t g_local[API_SLOTS];
#define LOCAL(f, how) (g_local[__builtin_offsetof(banana_api_t, f) / sizeof(void*)] = (how))

static void spec_init(void) {
    SPEC(exit, "i", 0);
    SPEC(write, "iBi", 0);
    SPEC(getchar, "", 0);
    SPEC(trygetchar, "", 0);
    SPEC(readline, "bi", 0);
    SPEC(set_color, "ii", 0);
    SPEC(clear_screen, "", 0);
    SPEC(term_size, "II", 0);
    SPEC(set_cursor, "ii", 0);
    SPEC(malloc, "i", 0);
    SPEC(free, "i", 0);                 /* (umem knows its own blocks) */
    SPEC(realloc, "ii", 0);
    SPEC(open, "si", 0);
    SPEC(read, "iBi", 0);
    SPEC(seek, "iii", 0);
    SPEC(close, "i", 0);
    SPEC(remove, "s", 0);
    SPEC(mkdir, "s", 0);
    SPEC(rename, "ss", 0);
    SPEC(stat, "so", sizeof(banana_stat_t));
    SPEC(readdir, "sio", sizeof(banana_dirent_t));
    SPEC(getcwd, "bi", 0);
    SPEC(chdir, "s", 0);
    SPEC(ticks_ms, "", 0);
    SPEC(sleep_ms, "i", 0);
    SPEC(yield, "", 0);
    SPEC(localtime, "o", sizeof(banana_time_t));
    SPEC(random, "", 0);
    SPEC(win_open, "sii", 0);
    SPEC(win_pixels, "i", 0);
    SPEC(win_update, "i", 0);
    SPEC(win_event, "io", sizeof(banana_event_t));
    SPEC(win_close, "i", 0);
    SPEC(win_set_title, "is", 0);
    SPEC(win_size, "iII", 0);
    SPEC(draw_text, "iiiiiisii", 0);    /* px: checked in it */
    SPEC(http_get, "sLLbibi", 0);
    SPEC(audio_play, "Biiii", 0);
    SPEC(audio_beep, "ii", 0);
    SPEC(audio_busy, "", 0);
    SPEC(clipboard_set, "Bi", 0);
    SPEC(clipboard_get, "L", 0);
    SPEC(interrupted, "", 0);
    SPEC(win_set_resizable, "iii", 0);
    SPEC(thread_create, "ii", 0);       /* fn: checked in it; arg: the app's business */
    SPEC(thread_join, "i", 0);
    SPEC(thread_id, "", 0);
    SPEC(thread_exit, "i", 0);
    SPEC(cpu_count, "", 0);
    SPEC(gpu_call, "ibibi", 0);
    SPEC(wait, "Iii", 0);
    SPEC(wake, "ii", 0);                /* (addr is only compared) */
    SPEC(web_open, "ii", 0);
    SPEC(web_close, "i", 0);
    SPEC(web_load, "is", 0);
    SPEC(web_load_html, "iss", 0);
    SPEC(web_resize, "iii", 0);
    SPEC(web_poll, "i", 0);
    SPEC(web_draw, "iiiiiii", 0);       /* px: checked in it */
    SPEC(web_event, "io", sizeof(banana_event_t));
    SPEC(web_scroll, "ii", 0);
    SPEC(web_go, "ii", 0);
    SPEC(web_info, "ibibi", 0);
    SPEC(web_eval, "isbi", 0);
    SPEC(web_message, "ibi", 0);
    SPEC(web_post, "is", 0);
    SPEC(font_draw, "iiiiiiiisi", 0);   /* px: checked in it */
    SPEC(font_width, "iis", 0);
    SPEC(font_metrics, "iiIII", 0);
    SPEC(audio_queued_ms, "", 0);
    SPEC(audio_stop, "", 0);
    SPEC(audio_volume, "i", 0);
    SPEC(win_media_keys, "i", 0);
    SPEC(http_request, "ssBisLLbibi", 0);
    SPEC(image_load, "sIIbi", 0);
    SPEC(set_wallpaper, "sibi", 0);
    SPEC(http_fetch, "ii", 0);          /* checked in it (the request points at more) */
    SPEC(key_mods, "", 0);
    SPEC(win_wheel, "i", 0);
    SPEC(win_capture_mouse, "ii", 0);
    SPEC(win_mouse_captured, "i", 0);
    SPEC(time_utc, "", 0);
    SPEC(utc_offset, "i", 0);
    SPEC(language, "bi", 0);
    SPEC(pkg_install, "sbi", 0);
    SPEC(app_run, "siibi", 0);          /* argv: checked in it */
    SPEC(repo_update, "bi", 0);
    SPEC(repo_list, "ii", 0);           /* out: checked in it */
    SPEC(repo_install, "sbi", 0);
    SPEC(repo_remove, "sbi", 0);
    SPEC(repo_status, "bi", 0);
    SPEC(repo_icon, "sbi", 0);
    SPEC(repo_upgrade, "bi", 0);
    SPEC(gamepad, "io", sizeof(banana_gamepad_t));
    SPEC(win_dropped, "iibi", 0);
    SPEC(key_down, "i", 0);
    SPEC(repo_sources, "ii", 0);        /* out: checked in it */
    SPEC(repo_add_source, "ssibi", 0);
    SPEC(repo_remove_source, "sbi", 0);
    SPEC(vm_map, "iiii", 0);            /* (addresses: checked in uvm) */
    SPEC(vm_unmap, "ii", 0);
    SPEC(vm_protect, "iii", 0);
    SPEC(thread_start, "iiii", 0);      /* fn, stack: checked in it */
    SPEC(thread_detach, "i", 0);
    SPEC(tls_set, "i", 0);              /* (only loaded into FS: the app's own business) */
    SPEC(mem_info, "LL", 0);
    SPEC(app_id, "", 0);
    SPEC(sock_open, "iii", 0);
    SPEC(sock_bind, "ibi", 0);
    SPEC(sock_listen, "ii", 0);
    SPEC(sock_accept, "iii", 0);          /* sa, len: checked in it */
    SPEC(sock_connect, "ibi", 0);
    SPEC(sock_send, "iBiiii", 0);         /* sa: checked in it */
    SPEC(sock_recv, "iBiiii", 0);         /* sa, salen: checked in it */
    SPEC(sock_shutdown, "ii", 0);
    SPEC(sock_opt, "iiiiii", 0);          /* val, len: checked in it */
    SPEC(sock_name, "iiii", 0);           /* sa, len: checked in it */
    SPEC(sock_ctl, "iii", 0);
    SPEC(poll, "iii", 0);                 /* fds: checked in it */
    SPEC(resolve, "si", 0);               /* ip: checked in it */
    SPEC(sig_entry, "i", 0);              /* checked in it */
    SPEC(sig_action, "iii", 0);
    SPEC(sig_mask, "iii", 0);             /* checked in it */
    SPEC(sig_send, "ii", 0);
    SPEC(sig_altstack, "ii", 0);          /* checked in it */
    SPEC(sig_pending, "", 0);

    LOCAL(ticks_ms, LOCAL_YES);
    LOCAL(cpu_count, LOCAL_YES);
    LOCAL(thread_id, LOCAL_YES);
    LOCAL(key_mods, LOCAL_YES);
    LOCAL(malloc, LOCAL_YES);
    LOCAL(free, LOCAL_YES);
    LOCAL(realloc, LOCAL_YES);
    LOCAL(draw_text, LOCAL_YES);
    LOCAL(wake, LOCAL_YES);             /* (task_wake only moves a deadline: any core may) */
    LOCAL(wait, LOCAL_WAIT);
    LOCAL(vm_map, LOCAL_YES);           /* (its own tables and memory, under their locks) */
    LOCAL(vm_unmap, LOCAL_YES);
    LOCAL(vm_protect, LOCAL_YES);
    LOCAL(app_id, LOCAL_YES);
    LOCAL(sig_mask, LOCAL_YES);           /* (the calling thread's own state) */
    LOCAL(sig_pending, LOCAL_YES);
}

/* the pointers of a call, against the app's memory (NULL is left to the call) */
static int args_ok(app_proc_t* p, const sysspec_t* sp, const uint64_t* a) {
    for (int i = 0; sp->args[i]; i++) {
        const void* ptr = (const void*)(uintptr_t)a[i];
        if (!ptr) continue;
        size_t n;
        switch (sp->args[i]) {
        case 's': if (!own_str(p, (const char*)ptr)) return 0; continue;
        case 'b': n = (int32_t)a[i + 1] > 0 ? (size_t)(int32_t)a[i + 1] : 1; break;
        case 'B': n = a[i + 1] ? (size_t)a[i + 1] : 1; if (a[i + 1] >> 32) return 0; break;
        case 'I': n = 4; break;
        case 'L': n = 8; break;
        case 'o': n = sp->osize; break;
        default: continue;
        }
        if (!own(p, ptr, n)) return 0;
    }
    return 1;
}

/* what kernel/isr64.asm saved (the layout of idt.c's registers_t) */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} sysframe_t;

/* ── signals (64-bit delivery) ─────────────────────────────────────────
 * A signal for a handler is delivered by changing the registers a thread
 * goes back to ring 3 with: a frame (siginfo, the registers it had in a
 * ucontext, its FPU/SSE state) is put on its stack - or its alternate
 * stack - and it resumes at the SDK's entry point, which calls the handler
 * and returns to the sigreturn stub: the kernel then takes the registers
 * back from the frame (maybe changed by the handler). */

#define SYS_SIGRETURN (API_SLOTS + 1)
#define STUB 16                         /* bytes per call stub in the app's table (make_uapi) */

static void __attribute__((noreturn)) bad_call(app_proc_t* p, uint64_t slot, const char* why);

/* Linux's register order in mcontext gregs */
enum { G_R8, G_R9, G_R10, G_R11, G_R12, G_R13, G_R14, G_R15, G_RDI, G_RSI, G_RBP, G_RBX, G_RDX,
       G_RAX, G_RCX, G_RSP, G_RIP, G_EFL, G_CSGSFS, G_ERR, G_TRAPNO, G_OLDMASK, G_CR2 };

typedef struct {
    banana_siginfo_t  info;
    banana_ucontext_t uc;
} sigbody_t;

static sigstate_t* sigst(app_proc_t* p) {
    athread_t* t = cur_thread(p);
    return t ? &t->sig : &p->msig;
}

/* sig for a handler now (the thread's registers in r): 1 if delivered */
static int sig_deliver(app_proc_t* p, sysframe_t* r, int sig, int code, uintptr_t addr, uint32_t trapno) {
    sigstate_t* st = sigst(p);
    if (!p->sig_entry || p->sig_disp[sig] != BANANA_SIG_CATCH) return 0;
    uint64_t sp = r->rsp;
    int on_alt = st->alt_size && sp - st->alt_sp < st->alt_size;
    if (((p->sig_onstack >> (sig - 1)) & 1) && st->alt_size && !on_alt) sp = st->alt_sp + st->alt_size;
    else sp -= 128;                                      /* (past any red zone) */
    uint64_t body_at = (sp - sizeof(sigbody_t)) & ~15ull;
    uint64_t top = body_at - 8;                          /* the return address: as after a call */
    if (!uvm_user_writable(p->us, p->mem, (uintptr_t)top, sizeof(sigbody_t) + 8)) return 0;
    sigbody_t* b = (sigbody_t*)(uintptr_t)body_at;
    memset(b, 0, sizeof(*b));
    b->info.si_signo = sig;
    b->info.si_code = code;
    b->info.si_addr = (void*)addr;
    b->info.si_pid = p->seq;
    b->info.si_uid = 1000;
    long long* g = b->uc.uc_mcontext.gregs;
    g[G_R8] = (long long)r->r8;   g[G_R9] = (long long)r->r9;   g[G_R10] = (long long)r->r10; g[G_R11] = (long long)r->r11;
    g[G_R12] = (long long)r->r12; g[G_R13] = (long long)r->r13; g[G_R14] = (long long)r->r14; g[G_R15] = (long long)r->r15;
    g[G_RDI] = (long long)r->rdi; g[G_RSI] = (long long)r->rsi; g[G_RBP] = (long long)r->rbp; g[G_RBX] = (long long)r->rbx;
    g[G_RDX] = (long long)r->rdx; g[G_RAX] = (long long)r->rax; g[G_RCX] = (long long)r->rcx; g[G_RSP] = (long long)r->rsp;
    g[G_RIP] = (long long)r->rip; g[G_EFL] = (long long)r->rflags; g[G_CSGSFS] = (long long)r->cs;
    g[G_ERR] = (long long)r->err_code; g[G_TRAPNO] = trapno; g[G_CR2] = (long long)addr;
    b->uc.uc_mcontext.fpregs = (void*)b->uc.fpregs_mem;
    b->uc.uc_sigmask[0] = st->blocked;
    b->uc.uc_stack.ss_sp = (void*)st->alt_sp;
    b->uc.uc_stack.ss_size = st->alt_size;
    b->uc.uc_stack.ss_flags = st->alt_size ? (on_alt ? 1 : 0) : 2;     /* SS_ONSTACK / SS_DISABLE */
    __asm__ volatile("fxsave64 (%0)" :: "r"(b->uc.fpregs_mem) : "memory");   /* (the app's: the kernel has no FPU code) */
    *(uint64_t*)(uintptr_t)top = p->sigret_stub;
    st->blocked |= 1ull << (sig - 1);                    /* (the SDK's entry sets sa_mask) */
    r->rip = p->sig_entry;
    r->rsp = top;
    r->rdi = (uint64_t)sig;
    r->rsi = (uint64_t)(uintptr_t)&b->info;
    r->rdx = (uint64_t)(uintptr_t)&b->uc;
    r->rax = 0;
    r->rflags &= ~0x500ull;                              /* TF and DF off, as the ABI wants */
    return 1;
}

/* the registers back from a handler's frame (sigreturn) */
static void sig_return(app_proc_t* p, sysframe_t* r) {
    sigbody_t* b = (sigbody_t*)(uintptr_t)r->rsp;        /* (the handler's ret popped the stub's address) */
    if (!own(p, b, sizeof(*b))) bad_call(p, SYS_SIGRETURN, "a signal frame that is not its own");
    const long long* g = b->uc.uc_mcontext.gregs;
    r->r8 = (uint64_t)g[G_R8];   r->r9 = (uint64_t)g[G_R9];   r->r10 = (uint64_t)g[G_R10]; r->r11 = (uint64_t)g[G_R11];
    r->r12 = (uint64_t)g[G_R12]; r->r13 = (uint64_t)g[G_R13]; r->r14 = (uint64_t)g[G_R14]; r->r15 = (uint64_t)g[G_R15];
    r->rdi = (uint64_t)g[G_RDI]; r->rsi = (uint64_t)g[G_RSI]; r->rbp = (uint64_t)g[G_RBP]; r->rbx = (uint64_t)g[G_RBX];
    r->rdx = (uint64_t)g[G_RDX]; r->rax = (uint64_t)g[G_RAX]; r->rcx = (uint64_t)g[G_RCX]; r->rsp = (uint64_t)g[G_RSP];
    r->rip = (uint64_t)g[G_RIP];
    /* only the arithmetic flags, DF, TF and AC are the app's to set */
    const uint64_t user = 0x40DD5ull;
    r->rflags = (r->rflags & ~user) | ((uint64_t)g[G_EFL] & user);
    /* the FPU/SSE state: MXCSR's reserved bits would fault in fxrstor */
    *(uint32_t*)(b->uc.fpregs_mem + 24) &= 0xFFBFu;
    __asm__ volatile("fxrstor64 (%0)" :: "r"(b->uc.fpregs_mem) : "memory");
    sigstate_t* st = sigst(p);
    st->blocked = b->uc.uc_sigmask[0] & ~((1ull << 8) | (1ull << 18));    /* (never SIGKILL, SIGSTOP) */
}

/* a pending signal the thread does not block, taken off the list: 0 if none */
static int sig_take(app_proc_t* p, sigstate_t* st) {
    uint64_t ready = (st->pending | p->sig_pending) & ~st->blocked;
    if (!ready) return 0;
    int sig = __builtin_ctzll(ready) + 1;
    uint64_t bit = 1ull << (sig - 1);
    if (st->pending & bit) st->pending &= ~bit;
    else p->sig_pending &= ~bit;
    return sig;
}

/* the thread goes back to ring 3 with registers r: its pending signals now */
static void sig_check(app_proc_t* p, sysframe_t* r) {
    for (int guard = 0; guard < 64; guard++) {
        sigstate_t* st = sigst(p);
        int sig = sig_take(p, st);
        if (!sig) return;
        int d = p->sig_disp[sig];
        if (d == BANANA_SIG_CATCH && sig_deliver(p, r, sig, -6 /* SI_TKILL */, 0, 0)) return;
        if (d == BANANA_SIG_IGNORE) continue;
        /* the default: these are ignored, the rest end the app */
        if (sig == 17 || sig == 18 || sig == 23 || sig == 28) continue;   /* CHLD CONT URG WINCH */
        ksnprintf(p->crash_msg, sizeof(p->crash_msg), "%s: ended by signal %d", p->name, sig);
        leave(p, 128 + sig);
    }
}

/* kernel/idt.c, a CPU exception in app code (ring 3): 1 if it went to the
 * app's handler as a signal (the CPU resumes there), 0 to stop the app */
int app_signal_fault(void* regs) {
    sysframe_t* r = (sysframe_t*)regs;
    app_proc_t* p = cur();
    if (!p || !p->sig_entry || (r->cs & 3) != 3) return 0;
    athread_t* t = cur_thread(p);
    if (t ? !t->running : !p->running) return 0;
    uint32_t v = (uint32_t)r->int_no;
    uintptr_t addr = 0;
    int sig, code;
    switch (v) {
    case 14:
        __asm__ volatile("mov %%cr2, %0" : "=r"(addr));
        sig = 11; code = (r->err_code & 1) ? 2 /* SEGV_ACCERR */ : 1 /* SEGV_MAPERR */;
        break;
    case 13: case 12: sig = 11; code = 128 /* SI_KERNEL */; break;
    case 0:  sig = 8; code = 1 /* FPE_INTDIV */; break;
    case 16: case 19: sig = 8; code = 0; break;
    case 6:  sig = 4; code = 2 /* ILL_ILLOPN */; addr = (uintptr_t)r->rip; break;
    case 1: case 3: sig = 5; code = 1 /* TRAP_BRKPT */; break;
    case 17: sig = 7; code = 1 /* BUS_ADRALN */; break;
    default: return 0;
    }
    if ((sigst(p)->blocked >> (sig - 1)) & 1) return 0;  /* a fault it blocks: it cannot go on */
    return sig_deliver(p, r, sig, code, addr, v);
}

/* the timer interrupted app code (kernel/idt.c via app_preempt) */
static void sig_preempt(app_proc_t* p, void* regs) {
    sysframe_t* r = (sysframe_t*)regs;
    if (!r || (r->cs & 3) != 3) return;
    sigstate_t* st = sigst(p);
    if ((st->pending | p->sig_pending) & ~st->blocked) sig_check(p, r);
}

typedef uint64_t (*sysfn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static void __attribute__((noreturn)) bad_call(app_proc_t* p, uint64_t slot, const char* why) {
    ksnprintf(p->crash_msg, sizeof(p->crash_msg), "%s: %s (system call %u) - it was stopped",
              p->name, why, (uint32_t)slot);
    leave(p, 139);
}

/* the arguments of a call: the first six from the registers, the rest
 * from the app's stack; 0 if they are not what its spec says */
static int gather_args(app_proc_t* p, const sysspec_t* sp, const sysframe_t* r, uint64_t* a) {
    a[0] = r->rdi; a[1] = r->rsi; a[2] = r->rdx; a[3] = r->rcx; a[4] = r->r8; a[5] = r->r9;
    for (int i = 6; i < SYS_ARGS; i++) a[i] = 0;
    uint32_t nargs = (uint32_t)strlen(sp->args);
    if (nargs > 6) {
        /* above the return address of the app's call */
        const uint64_t* st = (const uint64_t*)(uintptr_t)(r->rsp + 8);
        if (!own(p, st, (nargs - 6) * 8)) return 0;
        for (uint32_t i = 6; i < nargs; i++) a[i] = st[i - 6];
    }
    return args_ok(p, sp, a);
}

#define API_SLOT(f) (__builtin_offsetof(banana_api_t, f) / sizeof(void*))
static int in_uvm(uint64_t v) { return v >= UVM_BASE && v - UVM_BASE < UVM_SIZE; }

/* a call on another core, interrupts off: 1 if it was done here, 0 if it
 * must go to the boot core (where everything is checked again) */
static int syscall_here(sysframe_t* r) {
    uint64_t slot = r->rax;
    if (slot >= API_SLOTS || !g_local[slot] || !g_spec[slot].args) return 0;
    app_proc_t* p = cur();
    if (!p || p->exit_req || p->kill_req) return 0;     /* (the boot core ends it) */
    uint64_t a[SYS_ARGS];
    /* app memory in its mmap region may be in the swap area on the disk: a
     * call that would read it goes to the boot core, where a fault may wait
     * (interrupts are off here) - the vm_* calls only take its addresses.
     * Checked before the arguments are (that reads strings, the stack) */
    int vm = slot == API_SLOT(vm_map) || slot == API_SLOT(vm_unmap) || slot == API_SLOT(vm_protect);
    if (strlen(g_spec[slot].args) > 6 && in_uvm(r->rsp)) return 0;
    uint64_t reg[6] = { r->rdi, r->rsi, r->rdx, r->rcx, r->r8, r->r9 };
    uint32_t nargs = (uint32_t)strlen(g_spec[slot].args);
    if (!vm) for (uint32_t i = 0; i < nargs && i < 6; i++) if (in_uvm(reg[i])) return 0;   /* (the others hold what was there) */
    if (!gather_args(p, &g_spec[slot], r, a)) return 0;
    if (!vm) for (uint32_t i = 6; i < nargs && i < SYS_ARGS; i++) if (in_uvm(a[i])) return 0;
    if (g_local[slot] == LOCAL_WAIT) {
        /* the value changed already: nothing to wait for */
        if (!a[0] || *(volatile int*)(uintptr_t)a[0] == (int)a[1]) return 0;
        r->rax = 0;
        return 1;
    }
    sysfn_t fn = (sysfn_t)((void* const*)&g_api)[slot];
    r->rax = fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    return 1;
}

/* In a virtual machine, a core that runs app code may never leave the
 * guest: system calls made there stay there, and a busy app thread gives
 * the hypervisor no exit for seconds. VirtualBox on Hyper-V (its "NEM"
 * mode - every Windows PC with WSL 2) runs the machine's timers on such a
 * busy virtual CPU when the others halt: they waited for it, the boot
 * core's tick stopped, and the whole system froze while Minecraft ran.
 * So a core running app code exits to the hypervisor at least once a
 * millisecond, by a write to port 0x80 (the classic I/O delay: no device). */
static int      g_vm_exits = -1;                 /* -1: not known yet */
static uint64_t g_vm_exit_at[SMP_MAX_CPUS];
static uint64_t g_vm_probe_at[SMP_MAX_CPUS];
static unsigned g_vm_probe_calls[SMP_MAX_CPUS];

static void vm_exit_now_and_then(int cpu) {
    if (g_vm_exits < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        g_vm_exits = (c >> 31) & 1;              /* (a hypervisor) */
    }
    uint64_t per = timer_counter_per_ms();
    if (!g_vm_exits || !per || cpu <= 0 || cpu >= SMP_MAX_CPUS) return;
    /* Reading HPET/PM itself exits the VM. Avoid doing that on every
     * cheap syscall. Raw TSC is only a sampling hint here, never a clock
     * or deadline; backwards/jumping TSCs force an immediate resample,
     * and a call-count bound also resamples a frozen counter. */
    uint32_t lo,hi;
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    uint64_t stamp=(uint64_t)hi<<32 | lo;
    if(stamp-g_vm_probe_at[cpu]<100000 && ++g_vm_probe_calls[cpu]<32)return;
    g_vm_probe_at[cpu]=stamp;g_vm_probe_calls[cpu]=0;
    uint64_t now = timer_counter();
    if (now - g_vm_exit_at[cpu] < per) return;
    g_vm_exit_at[cpu] = now;
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

/* kernel/isr64.asm: int 0x80 (or syscall) from an app, on its thread's kernel stack */
void app_syscall(sysframe_t* r) {
    /* the kernel runs on the boot core: a call made elsewhere moves there,
     * unless it may run where it was made (interrupts are still off, as the
     * gate left them - and stay off for those) */
    if (cpu_id() != 0) {
        vm_exit_now_and_then(cpu_id());
        if ((r->cs & 3) == 3 && syscall_here(r)) return;
        task_ap_go_home();
    }
    __asm__ volatile("sti");
    app_proc_t* p = cur();
    uint64_t slot = r->rax;
    if (!p || (r->cs & 3) != 3) {               /* (only apps get here: a kernel bug otherwise) */
        r->rax = (uint64_t)-1;
        __asm__ volatile("cli");
        return;
    }
    if (slot == SYS_RETURN) {                   /* returned: what exit(ret) / thread_exit(ret) do */
        athread_t* t = cur_thread(p);
        if (t) leave_thread(p, t, (int)r->rdi);
        leave(p, (int)r->rdi);
    }
    if (slot == SYS_SIGRETURN) {                /* a signal handler returned */
        sig_return(p, r);
        r->int_no = 0x81;                       /* back by iretq: rcx / r11 too (sysret would not) */
        sig_check(p, r);
        __asm__ volatile("cli");
        return;
    }
    if (slot >= API_SLOTS || !g_spec[slot].args) bad_call(p, slot, "an unknown system call");
    uint64_t a[SYS_ARGS];
    if (!gather_args(p, &g_spec[slot], r, a)) bad_call(p, slot, "it passed memory that is not its own to the system");
    sysfn_t fn = (sysfn_t)((void* const*)&g_api)[slot];
    r->rax = fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    sig_check(p, r);                            /* signals sent to it go to their handlers now */
    __asm__ volatile("cli");
}

/* the call table an app gets, in its memory: the system's, every function
 * a stub; the font and the strings copied; the return stub after them */
static int make_uapi(app_proc_t* p) {
    size_t tab = (sizeof(banana_api_t) + 15) & ~(size_t)15;
    size_t stubs = API_SLOTS * STUB + 2 * STUB;     /* (+ the return and sigreturn stubs) */
    uint8_t* m = (uint8_t*)umem_alloc_pages(p->mem, tab + stubs + sizeof(font8x8_basic) + 64, 0);
    if (!m) return -1;
    banana_api_t* ua = (banana_api_t*)m;
    memcpy(ua, &g_api, sizeof(g_api));
    void** slot = (void**)ua;
    uint8_t* st = m + tab;
    int sc = idt_has_syscall();
    for (uint32_t k = 0; k < API_SLOTS; k++, st += STUB) {
        memset(st, 0xCC, STUB);                         /* (int3 in the gaps) */
        if (sc) {
            /* mov r10, rcx; mov eax, k; syscall; ret - syscall keeps the
             * return address in rcx, so the 4th argument goes in r10 */
            st[0] = 0x49; st[1] = 0x89; st[2] = 0xCA;
            st[3] = 0xB8; memcpy(st + 4, &k, 4);
            st[8] = 0x0F; st[9] = 0x05;
            st[10] = 0xC3;
        } else {
            st[0] = 0xB8;                               /* mov eax, k */
            memcpy(st + 1, &k, 4);
            st[5] = 0xCD; st[6] = SYSCALL_VECTOR;       /* int 0x80 */
            st[7] = 0xC3;                               /* ret */
        }
        if (g_spec[k].args) slot[k] = st;
    }
    p->ret_stub = (uintptr_t)(m + tab) + SYS_RETURN * STUB;
    /* A C function returns its int in eax. Preserve it as the syscall's
     * first argument BEFORE loading the syscall number into eax. Ordinary
     * call stubs leave rdi alone; it is not the function's return value. */
    memset(st, 0xCC, 2 * STUB);
    st[0] = 0x89; st[1] = 0xC7;                         /* mov edi, eax */
    st[2] = 0xB8;                                      /* mov eax, SYS_RETURN */
    uint32_t ret_slot = SYS_RETURN;
    memcpy(st + 3, &ret_slot, 4);
    if (sc) { st[7] = 0x0F; st[8] = 0x05; }            /* syscall */
    else { st[7] = 0xCD; st[8] = SYSCALL_VECTOR; }      /* int 0x80 */
    st[9] = 0x0F; st[10] = 0x0B;                        /* ud2: must not return */
    /* where a signal handler returns to (sig_deliver): the frame is under rsp
     * (rcx and r11, which syscall overwrites, come back from it: iretq) */
    st += STUB;
    st[0] = 0xB8;                                       /* mov eax, SYS_SIGRETURN */
    uint32_t sr_slot = SYS_SIGRETURN;
    memcpy(st + 1, &sr_slot, 4);
    if (sc) { st[5] = 0x0F; st[6] = 0x05; }
    else { st[5] = 0xCD; st[6] = SYSCALL_VECTOR; }
    st[7] = 0x0F; st[8] = 0x0B;                         /* ud2: does not return here */
    p->sigret_stub = (uintptr_t)st;
    uint8_t* font = m + tab + stubs;
    memcpy(font, font8x8_basic, sizeof(font8x8_basic));
    ua->font8x8 = font;
    char* str = (char*)font + sizeof(font8x8_basic);
    kstrlcpy(str, g_api.arch, 16);
    ua->arch = str;
    kstrlcpy(str + 16, g_api.os_version, 16);
    ua->os_version = str + 16;
    p->uapi = ua;
    return 0;
}
#endif

/* (64-bit: every entry of the app's table is a system call stub instead -
 * app_syscall below; G stays for the 32-bit kernel's direct calls) */
#define G(fn) (fn)

static void api_init(void) {
    if (g_api.magic) return;
    g_api.magic = BANANA_API_MAGIC;
    g_api.version = BANANA_API_VERSION;
    g_api.size = sizeof(banana_api_t);
    g_api.arch = BANANA_ARCH;
    g_api.os_version = "0.6";
    g_api.exit = G(a_exit);
    g_api.write = G(a_write);
    g_api.getchar = G(a_getchar);
    g_api.trygetchar = G(a_trygetchar);
    g_api.readline = G(a_readline);
    g_api.set_color = G(a_set_color);
    g_api.clear_screen = G(a_clear);
    g_api.term_size = G(a_term_size);
    g_api.set_cursor = G(a_set_cursor);
    g_api.malloc = G(a_malloc);
    g_api.free = G(a_free);
    g_api.realloc = G(a_realloc);
    g_api.open = G(a_open);
    g_api.read = G(a_read);
    g_api.seek = G(a_seek);
    g_api.close = G(a_close);
    g_api.remove = G(a_remove);
    g_api.mkdir = G(a_mkdir);
    g_api.rename = G(a_rename);
    g_api.stat = G(a_stat);
    g_api.readdir = G(a_readdir);
    g_api.getcwd = G(a_getcwd);
    g_api.chdir = G(a_chdir);
    g_api.ticks_ms = a_ticks;
    g_api.sleep_ms = G(a_sleep);
    g_api.yield = G(a_yield);
    g_api.localtime = G(a_localtime);
    g_api.random = G(a_random);
    g_api.win_open = G(a_win_open);
    g_api.win_pixels = G(a_win_pixels);
    g_api.win_update = G(a_win_update);
    g_api.win_event = G(a_win_event);
    g_api.win_close = G(a_win_close);
    g_api.win_set_title = G(a_win_set_title);
    g_api.win_size = G(a_win_size);
    g_api.draw_text = a_draw_text;
    g_api.font8x8 = &font8x8_basic[0][0];
    g_api.http_get = G(a_http_get);
    g_api.audio_play = G(a_audio_play);
    g_api.audio_beep = G(a_audio_beep);
    g_api.audio_busy = G(a_audio_busy);
    g_api.clipboard_set = G(a_clip_set);
    g_api.clipboard_get = G(a_clip_get);
    g_api.interrupted = G(a_interrupted);
    g_api.win_set_resizable = G(a_win_set_resizable);
    g_api.thread_create = G(a_thread_create);
    g_api.thread_join = G(a_thread_join);
    g_api.thread_id = G(a_thread_id);
    g_api.thread_exit = G(a_thread_exit);
    g_api.cpu_count = a_cpu_count;
    g_api.gpu_call = a_gpu_call;
    g_api.wait = G(a_wait);
    g_api.wake = G(a_wake);
    g_api.web_open = G(a_web_open);
    g_api.web_close = G(a_web_close);
    g_api.web_load = G(a_web_load);
    g_api.web_load_html = G(a_web_load_html);
    g_api.web_resize = G(a_web_resize);
    g_api.web_poll = G(a_web_poll);
    g_api.web_draw = G(a_web_draw);
    g_api.web_event = G(a_web_event);
    g_api.web_scroll = G(a_web_scroll);
    g_api.web_go = G(a_web_go);
    g_api.web_info = G(a_web_info);
    g_api.web_eval = G(a_web_eval);
    g_api.web_message = G(a_web_message);
    g_api.web_post = G(a_web_post);
    g_api.font_draw = G(a_font_draw);
    g_api.font_width = G(a_font_width);
    g_api.font_metrics = G(a_font_metrics);
    g_api.audio_queued_ms = G(a_audio_queued_ms);
    g_api.audio_stop = G(a_audio_stop);
    g_api.audio_volume = G(a_audio_volume);
    g_api.win_media_keys = G(a_win_media_keys);
    g_api.http_request = G(a_http_request);
    g_api.image_load = G(a_image_load);
    g_api.set_wallpaper = G(a_set_wallpaper);
    g_api.http_fetch = G(a_http_fetch);
    g_api.key_mods = G(a_key_mods);
    g_api.win_wheel = G(a_win_wheel);
    g_api.win_capture_mouse = G(a_win_capture_mouse);
    g_api.win_mouse_captured = G(a_win_mouse_captured);
    g_api.time_utc = G(a_time_utc);
    g_api.utc_offset = G(a_utc_offset);
    g_api.language = G(a_language);
    g_api.pkg_install = G(a_pkg_install);
    g_api.app_run = G(a_app_run);
    g_api.repo_update = G(a_repo_update);
    g_api.repo_list = G(a_repo_list);
    g_api.repo_install = G(a_repo_install);
    g_api.repo_remove = G(a_repo_remove);
    g_api.repo_status = G(a_repo_status);
    g_api.repo_icon = G(a_repo_icon);
    g_api.repo_upgrade = G(a_repo_upgrade);
    g_api.gamepad = G(a_gamepad);
    g_api.win_dropped = G(a_win_dropped);
    g_api.key_down = G(a_key_down);
    g_api.repo_sources = G(a_repo_sources);
    g_api.repo_add_source = G(a_repo_add_source);
    g_api.repo_remove_source = G(a_repo_remove_source);
    g_api.vm_map = G(a_vm_map);
    g_api.vm_unmap = G(a_vm_unmap);
    g_api.vm_protect = G(a_vm_protect);
    g_api.thread_start = G(a_thread_start);
    g_api.thread_detach = G(a_thread_detach);
    g_api.tls_set = G(a_tls_set);
    g_api.mem_info = G(a_mem_info);
    g_api.app_id = G(a_app_id);
    g_api.sock_open = G(a_sock_open);
    g_api.sock_bind = G(a_sock_bind);
    g_api.sock_listen = G(a_sock_listen);
    g_api.sock_accept = G(a_sock_accept);
    g_api.sock_connect = G(a_sock_connect);
    g_api.sock_send = G(a_sock_send);
    g_api.sock_recv = G(a_sock_recv);
    g_api.sock_shutdown = G(a_sock_shutdown);
    g_api.sock_opt = G(a_sock_opt);
    g_api.sock_name = G(a_sock_name);
    g_api.sock_ctl = G(a_sock_ctl);
    g_api.poll = G(a_poll);
    g_api.resolve = G(a_resolve);
    g_api.sig_entry = G(a_sig_entry);
    g_api.sig_action = G(a_sig_action);
    g_api.sig_mask = G(a_sig_mask);
    g_api.sig_send = G(a_sig_send);
    g_api.sig_altstack = G(a_sig_altstack);
    g_api.sig_pending = G(a_sig_pending);
#ifdef __x86_64__
    spec_init();
#endif
}

/* ── the ELF loader ────────────────────────────────────────────────── */

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
#define DT_REL     17
#define DT_RELSZ   18
#define DT_RELENT  19
#define DT_PLTRELSZ 2
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_SYMENT  11
#define DT_PLTREL  20
#define DT_JMPREL  23
#define R_X86_64_NONE     0
#define R_X86_64_RELATIVE 8
#define R_386_NONE        0
#define R_386_RELATIVE    8

#ifdef __x86_64__
#define EM_HOST 62            /* EM_X86_64 */
#define ELF_CLASS 2
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } ehdr_t;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } phdr_t;
typedef struct { int64_t tag; uint64_t val; } dyn_t;
#else
#define EM_HOST 3             /* EM_386 */
#define ELF_CLASS 1
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } ehdr_t;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } phdr_t;
typedef struct { int32_t tag; uint32_t val; } dyn_t;
#endif


/* m: the app's memory to load it into (NULL: the kernel's, for a driver) */
static int load_elf(umem_t* m, const uint8_t* f, uint32_t size, uint8_t** image_out, banana_entry_t* entry, uintptr_t* base_out,
                    uintptr_t* lo_out, uintptr_t* hi_out, char* err, int ecap) {
    const ehdr_t* eh = (const ehdr_t*)f;
    if (size < sizeof(ehdr_t) || memcmp(eh->ident, "\x7f" "ELF", 4) != 0) { kstrlcpy(err, "not an ELF program", (size_t)ecap); return -1; }
    if (eh->ident[4] != ELF_CLASS || eh->machine != EM_HOST) {
        ksnprintf(err, (size_t)ecap, "built for another CPU (this is the %s kernel)", BANANA_ARCH);
        return -1;
    }
    if (eh->type != 3) { kstrlcpy(err, "not position-independent (build it with the Banana OS SDK)", (size_t)ecap); return -1; }
    if (eh->phentsize != sizeof(phdr_t) || eh->phoff + (uint32_t)eh->phnum * sizeof(phdr_t) > size) {
        kstrlcpy(err, "damaged ELF headers", (size_t)ecap);
        return -1;
    }
    const phdr_t* ph = (const phdr_t*)(f + eh->phoff);
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].offset + ph[i].filesz > size || ph[i].filesz > ph[i].memsz) { kstrlcpy(err, "damaged ELF segments", (size_t)ecap); return -1; }
        if (ph[i].vaddr < lo) lo = (uintptr_t)ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = (uintptr_t)(ph[i].vaddr + ph[i].memsz);
    }
    if (hi <= lo || hi - lo > APP_MAX_IMAGE) { kstrlcpy(err, "no loadable code (or too big)", (size_t)ecap); return -1; }
    lo &= ~(uintptr_t)4095;
    uint32_t span = (uint32_t)(hi - lo);
    uint8_t* raw = m ? (uint8_t*)umem_alloc_pages(m, span, 0) : (uint8_t*)kmalloc(span + 4096);
    if (!raw) { kstrlcpy(err, "out of memory", (size_t)ecap); return -1; }
    uint8_t* mem = (uint8_t*)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    memset(mem, 0, span);
    uintptr_t base = (uintptr_t)mem - lo;
    const dyn_t* dyn = NULL;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_LOAD) memcpy((void*)(base + (uintptr_t)ph[i].vaddr), f + ph[i].offset, (size_t)ph[i].filesz);
        if (ph[i].type == PT_DYNAMIC) dyn = (const dyn_t*)(base + (uintptr_t)ph[i].vaddr);
    }
    /* relocations: the SDK's static PIEs only have "base + addend" ones;
     * TinyCC's programs (-shared, built on Banana OS) also refer to their
     * own symbols - through the GOT and PLT, and (i386) in their code */
    if (dyn) {
        uintptr_t rel = 0, relsz = 0, relent = 0, rela = 0, relasz = 0, relaent = 0;
        uintptr_t symtab = 0, syment = 0, strtab = 0, jmprel = 0, pltrelsz = 0, pltrel = 0;
        for (const dyn_t* d = dyn; d->tag != DT_NULL && (const uint8_t*)d < mem + span; d++) {
            switch (d->tag) {
            case DT_RELA: rela = (uintptr_t)d->val; break;
            case DT_RELASZ: relasz = (uintptr_t)d->val; break;
            case DT_RELAENT: relaent = (uintptr_t)d->val; break;
            case DT_REL: rel = (uintptr_t)d->val; break;
            case DT_RELSZ: relsz = (uintptr_t)d->val; break;
            case DT_RELENT: relent = (uintptr_t)d->val; break;
            case DT_SYMTAB: symtab = (uintptr_t)d->val; break;
            case DT_SYMENT: syment = (uintptr_t)d->val; break;
            case DT_STRTAB: strtab = (uintptr_t)d->val; break;
            case DT_JMPREL: jmprel = (uintptr_t)d->val; break;
            case DT_PLTRELSZ: pltrelsz = (uintptr_t)d->val; break;
            case DT_PLTREL: pltrel = (uintptr_t)d->val; break;
            }
        }
#define IN_IMAGE(a, n) ((a) >= lo && (a) - lo + (n) <= span)
        int bad = 0;
        char why[96] = "";
#ifdef __x86_64__
        (void)rel; (void)relsz; (void)relent;
        if (!syment) syment = 24;
        uintptr_t tabs[2][2] = { { rela, relasz }, { pltrel == DT_RELA ? jmprel : 0, pltrelsz } };
        if (rela && relaent && relaent != 24) bad = 1;
        for (int t = 0; t < 2 && !bad; t++) {
            if (!tabs[t][0]) continue;
            if (!IN_IMAGE(tabs[t][0], tabs[t][1])) { bad = 1; break; }
            for (uintptr_t o = 0; o + 24 <= tabs[t][1]; o += 24) {
                const uint64_t* r = (const uint64_t*)(base + tabs[t][0] + o);
                uint32_t type = (uint32_t)r[1], si = (uint32_t)(r[1] >> 32);
                if (type == R_X86_64_NONE) continue;
                if (!IN_IMAGE(r[0], 8)) { bad = 1; break; }
                uint64_t* P = (uint64_t*)(base + r[0]);
                if (type == R_X86_64_RELATIVE) { *P = base + r[2]; continue; }
                /* a symbol of the program itself */
                uintptr_t sa = symtab + (uintptr_t)si * syment;
                if (!symtab || !IN_IMAGE(sa, 24)) { bad = 1; break; }
                const uint8_t* sym = (const uint8_t*)(base + sa);
                uint16_t shndx = *(const uint16_t*)(sym + 6);
                uint64_t val = *(const uint64_t*)(sym + 8);
                if (!shndx) {
                    uint32_t nm = *(const uint32_t*)sym;
                    ksnprintf(why, sizeof(why), "it needs %s, which Banana OS does not have",
                              strtab && IN_IMAGE(strtab + nm, 1) ? (const char*)(base + strtab + nm) : "a missing function");
                    bad = 2;
                    break;
                }
                uint64_t S = base + val;
                if (type == 1) *P = S + r[2];                          /* R_X86_64_64 */
                else if (type == 6 || type == 7) *P = S;               /* GLOB_DAT, JUMP_SLOT */
                else if (type == 2) *(uint32_t*)P = (uint32_t)(S + r[2] - (uint64_t)(uintptr_t)P);   /* PC32 */
                else { ksnprintf(why, sizeof(why), "unsupported relocation type %u", type); bad = 2; break; }
            }
        }
#else
        (void)rela; (void)relasz; (void)relaent;
        if (!syment) syment = 16;
        uintptr_t tabs[2][2] = { { rel, relsz }, { pltrel == DT_REL ? jmprel : 0, pltrelsz } };
        if (rel && relent && relent != 8) bad = 1;
        for (int t = 0; t < 2 && !bad; t++) {
            if (!tabs[t][0]) continue;
            if (!IN_IMAGE(tabs[t][0], tabs[t][1])) { bad = 1; break; }
            for (uintptr_t o = 0; o + 8 <= tabs[t][1]; o += 8) {
                const uint32_t* r = (const uint32_t*)(base + tabs[t][0] + o);
                uint32_t type = r[1] & 0xFF, si = r[1] >> 8;
                if (type == R_386_NONE) continue;
                if (!IN_IMAGE(r[0], 4)) { bad = 1; break; }
                uint32_t* P = (uint32_t*)(base + r[0]);
                if (type == R_386_RELATIVE) { *P += base; continue; }
                uintptr_t sa = symtab + (uintptr_t)si * syment;
                if (!symtab || !IN_IMAGE(sa, 16)) { bad = 1; break; }
                const uint8_t* sym = (const uint8_t*)(base + sa);
                uint32_t val = *(const uint32_t*)(sym + 4);
                uint16_t shndx = *(const uint16_t*)(sym + 14);
                if (!shndx) {
                    uint32_t nm = *(const uint32_t*)sym;
                    ksnprintf(why, sizeof(why), "it needs %s, which Banana OS does not have",
                              strtab && IN_IMAGE(strtab + nm, 1) ? (const char*)(base + strtab + nm) : "a missing function");
                    bad = 2;
                    break;
                }
                uint32_t S = (uint32_t)base + val;
                if (type == 1) *P += S;                                /* R_386_32 */
                else if (type == 2) *P += S - (uint32_t)(uintptr_t)P;  /* R_386_PC32 */
                else if (type == 6 || type == 7) *P = S;               /* GLOB_DAT, JMP_SLOT */
                else { ksnprintf(why, sizeof(why), "unsupported relocation type %u", type); bad = 2; break; }
            }
        }
#endif
#undef IN_IMAGE
        if (bad) {
            if (bad == 1) kstrlcpy(err, "damaged relocations (link it with the SDK or tcc)", (size_t)ecap);
            else kstrlcpy(err, why, (size_t)ecap);
            if (m) umem_free_pages(m, raw, 0);
            else kfree(raw);
            return -1;
        }
    }
    *image_out = raw;
    *entry = (banana_entry_t)(base + (uintptr_t)eh->entry);
    *base_out = base;
    *lo_out = (uintptr_t)mem;
    *hi_out = (uintptr_t)mem + span;
    return 0;
}

/* the same loader for driver modules (kernel/driver.c): the image stays loaded */
int app_load_image(const uint8_t* f, uint32_t size, uint8_t** raw, uintptr_t* entry, char* err, int ecap) {
    banana_entry_t e;
    uintptr_t base, lo, hi;
    if (load_elf(NULL, f, size, raw, &e, &base, &lo, &hi, err, ecap) < 0) return -1;
    *entry = (uintptr_t)e;
    return 0;
}

/* ── running ───────────────────────────────────────────────────────── */

#ifndef __x86_64__
static int trampoline(void* arg) {
    app_proc_t* p = (app_proc_t*)arg;
    return p->entry(&g_api, p->argc, p->argv);
}
#endif

/* before an app's memory goes: every thread has to be out of its code */
static void stop_threads(app_proc_t* p) {
    p->exit_req = 1;
    for (;;) {
        int live = 0;
        for (int t = 0; t < APP_THREADS; t++) if (p->threads[t].used && !p->threads[t].done) live++;
        if (!live) break;
        task_sleep_ms(2);                   /* a busy one is stopped by the timer (app_preempt) */
    }
    for (int t = 0; t < APP_THREADS; t++) {
        if (p->threads[t].used) thread_free(p, &p->threads[t]);
        p->threads[t].used = 0;
    }
}

/* everything the app had goes back (no task runs on its tables any more) */
static void discard(app_proc_t* p) {
#ifdef __x86_64__
    kstack_free(p->kstack);
    p->kstack = NULL;
#endif
    umem_destroy(p->mem);                  /* its code, stacks, memory, argv */
    uspace_destroy(p->us);
    p->mem = NULL;
    p->us = NULL;
}

static void release(app_proc_t* p) {
    stop_threads(p);
    for (int i = 0; i < APP_FD_MAX; i++)            /* its sockets close (connections end) */
        if (p->fds[i].used && p->fds[i].sock) { usock_close(p->fds[i].sock); p->fds[i].sock = NULL; }
        else if (p->fds[i].used && p->fds[i].fidx >= 0) { fs_unpin(p->fds[i].fidx); p->fds[i].used = 0; }
    gpu_virgl_release(p->id + 1);
    appwin_close_owner(p->id);              /* (their pictures are its memory: before it goes) */
    webview_close_owner(p->id);
    discard(p);
    pid_clear(p->pid, p->id);
    p->used = 0;
}


static int run(app_proc_t* p) {
    *(volatile uint32_t*)p->stack = STACK_CANARY;      /* the bottom of the stack: overflow check */
    p->running = 1;
#ifdef __x86_64__
    p->guarded = 1;                                     /* (its guard pages: umem_alloc_pages) */
    p->exit_code = enter_user(p, (uintptr_t)p->entry, (uintptr_t)p->uapi, (uintptr_t)p->argc,
                              (uintptr_t)p->argv, p->stack, APP_STACK, p->kstack, &p->saved_sp);
#else
    p->exit_code = app_enter(trampoline, p, p->stack + APP_STACK, &p->saved_sp);
#endif
    p->running = 0;
    if (p->crash_msg[0]) {                              /* it crashed (app_fault) */
        klog("app crashed: %s\n", p->crash_msg);
        if (p->has_term) {
            terminal_vt_set_active(p->vt);
            terminal_write_color(p->crash_msg, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_putchar('\n');
        }
    }
    if (*(volatile uint32_t*)p->stack != STACK_CANARY)
        klog("app %s: its stack overflowed (256 KiB) - memory may be damaged\n", p->name);
    return p->exit_code;
}

/* A CPU exception (kernel/idt.c) while an app runs - in its own code, or
 * in a system call it made with a bad pointer: the app is stopped, the
 * rest of the system goes on. Returns only if no app is running here. */
void app_fault(uint32_t vector, uint32_t err, uintptr_t ip, uintptr_t addr) {
    app_proc_t* p = cur();
    if (!p) return;
    athread_t* ft = cur_thread(p);
    if (ft ? !ft->running : !p->running) return;
    if (ft) ft->running = 0;
    else p->running = 0;
    /* We came in through an interrupt gate, so interrupts are off - and the
     * terminal output below may yield to other tasks, which must not run
     * with interrupts off (timers, sleeps and hlt would all stop): turn
     * them on first. This exception handler is never returned from. */
    __asm__ volatile("sti");
    static const char* const what[20] = {
        "division by zero", "debug trap", "NMI", "breakpoint", "overflow", "bound range",
        "invalid instruction", "no FPU", "double fault", "", "invalid TSS", "segment not present",
        "stack fault", "general protection fault", "segmentation fault (bad memory access)", "",
        "floating point error", "alignment check", "machine check", "SIMD floating point error",
    };
    const char* w = vector < 20 && what[vector][0] ? what[vector] : "CPU exception";
    uintptr_t base = p->base;
    char line[160];
    int overflow = p->guarded && addr >= (uintptr_t)p->guard && addr < (uintptr_t)p->guard + APP_GUARD;
    for (int k = 0; k < APP_THREADS; k++) {
        const athread_t* th = &p->threads[k];
        if (p->guarded && th->used && th->stack && addr >= (uintptr_t)th->stack - APP_GUARD && addr < (uintptr_t)th->stack)
            overflow = 1;
    }
    if (vector == 14 && overflow)
        ksnprintf(line, sizeof(line), "%s: stack overflow - it used more than %u KiB of stack (big local arrays? malloc them)",
                  p->name, APP_STACK >> 10);
    else if (vector == 14)
        ksnprintf(line, sizeof(line), "%s: %s at address %p (ip %p, app+%x, error %x)", p->name, w, (void*)addr,
                  (void*)ip, (uint32_t)(ip - base), err);
    else
        ksnprintf(line, sizeof(line), "%s: %s (ip %p, app+%x)", p->name, w, (void*)ip, (uint32_t)(ip - base));
    /* reported by run(), back on the task's own stack: this may be the
     * dedicated fault stack, which must not be in use when we yield */
    kstrlcpy(p->crash_msg, line, sizeof(p->crash_msg));
    athread_t* t = cur_thread(p);
    if (t) {                                /* a crash in a thread ends the whole app */
        if (!p->exit_req) { p->exit_req = 1; p->exit_req_code = 139; }
        t->running = 0;
        app_leave(t->saved_sp, 139);
    }
    app_leave(p->saved_sp, 139);
}

/* reads, loads and prepares an app (slot claimed, not started) */
static app_proc_t* prepare(const char* path, int argc, char** argv, char* err, int ecap) {
    api_init();
    int fi = fs_find_file(path);
    if (fi < 0) { kstrlcpy(err, "no such program", (size_t)ecap); return NULL; }
    app_proc_t* p = NULL;
    for (int i = 0; i < APP_MAX; i++) if (!g_procs[i].used) { p = &g_procs[i]; p->id = i; break; }
    if (!p) { kstrlcpy(err, "too many apps running", (size_t)ecap); return NULL; }
    int id = p->id;
    memset(p, 0, sizeof(*p));
    p->id = id;
    static int s_seq = 100;
    p->seq = ++s_seq;
    fs_file_t* f = fs_get_file(fi);
    if (!f) { kstrlcpy(err, "cannot read it", (size_t)ecap); return NULL; }
#ifdef __x86_64__
    p->us = uspace_create();
    if (!p->us) { kstrlcpy(err, "out of memory", (size_t)ecap); return NULL; }
#endif
    p->mem = umem_create(p->us);
    if (!p->mem) { discard(p); kstrlcpy(err, "out of memory", (size_t)ecap); return NULL; }
    uint8_t* image;
    fs_pin(fi);
    int bad = load_elf(p->mem, (const uint8_t*)f->content, f->size, &image, &p->entry, &p->base, &p->code_lo, &p->code_hi, err, ecap) != 0;
    fs_unpin(fi);
    if (bad) { discard(p); return NULL; }
    /* its stack, guard pages under it (64-bit: they fault) */
    p->stack = (uint8_t*)umem_alloc_pages(p->mem, APP_STACK, APP_GUARD);
    p->guard = p->stack ? p->stack - APP_GUARD : NULL;
    /* argv: the pointers and the strings in one block of its memory */
    uint32_t need = (uint32_t)(argc + 1) * sizeof(char*);
    for (int i = 0; i < argc; i++) need += (uint32_t)strlen(argv[i]) + 1;
    p->argv = (char**)umem_alloc(p->mem, need);
    int ok = p->stack && p->argv;
#ifdef __x86_64__
    p->kstack = kstack_new();
    ok = ok && p->kstack && make_uapi(p) == 0;
#endif
    if (!ok) {
        discard(p);
        kstrlcpy(err, "out of memory", (size_t)ecap);
        return NULL;
    }
    char* s = (char*)(p->argv + argc + 1);
    for (int i = 0; i < argc; i++) {
        p->argv[i] = s;
        size_t l = strlen(argv[i]) + 1;
        memcpy(s, argv[i], l);
        s += l;
    }
    p->argv[argc] = NULL;
    p->argc = argc;
    const char* base = strrchr(argc ? argv[0] : path, '/');
    kstrlcpy(p->name, base ? base + 1 : (argc ? argv[0] : path), sizeof(p->name));
    p->used = 1;
    return p;
}


int app_exec(const char* path, int argc, char** argv, char* err, int ecap) {
    app_proc_t* p = prepare(path, argc, argv, err, ecap);
    if (!p) return -1;
    p->pid = task_current_pid();
    pid_set(p->pid, p->id, -1);
    p->vt = terminal_vt_get_active();
    p->has_term = !task_is_background();
    p->last_breath = timer_ms();
    int rc = run(p);
    terminal_vt_set_active(p->vt);
    terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    release(p);
    return rc;
}

static void spawned_entry(void) {
    app_proc_t* p = cur();
    if (!p) return;
    task_set_background();             /* no terminal: it talks through its windows */
    run(p);
    klog("app %s exited (%d)\n", p->name, p->exit_code);
    release(p);
}

int app_spawn(const char* path, int argc, char** argv, char* err, int ecap) {
    app_proc_t* p = prepare(path, argc, argv, err, ecap);
    if (!p) return -1;
    p->pid = -1;
    p->own_task = 1;
    p->has_term = 0;
    p->vt = 0;
    int pid = task_create(p->name, spawned_entry);
    if (pid < 0) { release(p); kstrlcpy(err, "too many tasks", (size_t)ecap); return -1; }
    p->pid = pid;                      /* the new task runs once we yield */
    pid_set(pid, p->id, -1);
    return 0;
}

/* what an app holds: its blocks' memory, and (64-bit) the pages of its
 * mapped memory in RAM - where its malloc heap is now (sdk/lib/malloc.c) */
static uint32_t app_mem_bytes(app_proc_t* p) {
    uint64_t n = umem_used(p->mem);
#ifdef __x86_64__
    n += uvm_resident(p->us);
#endif
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

int app_count(void) {
    int n = 0;
    for (int i = 0; i < APP_MAX; i++) if (g_procs[i].used) n++;
    return n;
}

void app_list(void) {
    char line[96];
    int any = 0;
    for (int i = 0; i < APP_MAX; i++) {
        app_proc_t* p = &g_procs[i];
        if (!p->used) continue;
        uint32_t mem = app_mem_bytes(p);
        ksnprintf(line, sizeof(line), "  %-16s pid %-3d %s  %u KiB allocated", p->name, p->pid,
                  p->own_task ? "desktop " : "terminal", mem / 1024);
        terminal_writeln(line);
        any = 1;
    }
    if (!any) terminal_writeln("  (no app running)");
}

int app_snapshot(app_info_t* out, int max) {
    int n = 0;
    for (int i = 0; i < APP_MAX && n < max; i++) {
        app_proc_t* p = &g_procs[i];
        if (!p->used) continue;
        out[n].id = p->id;
        kstrlcpy(out[n].name, p->name, sizeof(out[n].name));
        out[n].pid = p->pid;
        out[n].desktop = p->own_task;
        out[n].mem = app_mem_bytes(p);
        out[n].threads = 1;
        for (int t = 0; t < APP_THREADS; t++) if (p->threads[t].used && !p->threads[t].done) out[n].threads++;
        n++;
    }
    return n;
}

int app_kill(int id) {
    if (id < 0 || id >= APP_MAX || !g_procs[id].used) return -1;
    g_procs[id].kill_req = 1;
    return 0;
}

int app_console_focused(void) {
    for (int i = 0; i < APP_MAX; i++)
        if (g_procs[i].used && focused(&g_procs[i])) return 1;
    return 0;
}
