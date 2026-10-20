/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "termrender.h"
#include "settings.h"
#include "gpu_virgl.h"
#include "kstring.h"
#include "serial.h"

/* ── the settings ── */
static int      g_gpu = -1, g_method = -1;        /* -1: not read yet */
static uint32_t g_gen;
static termrender_stats_t g_stats;

static void load(void) {
    if (g_gpu >= 0) return;
    char v[16];
    g_gpu = 1;                                     /* (on, the cells renderer: the default) */
    g_method = TR_CELLS;
    if (settings_get("term_gpu", v, sizeof(v))) g_gpu = strcmp(v, "off") != 0;
    if (settings_get("term_renderer", v, sizeof(v))) g_method = strcmp(v, "virgl") ? TR_CELLS : TR_VIRGL;
}

int  termrender_gpu(void) { load(); return g_gpu; }
int  termrender_method(void) { load(); return g_method; }
const char* termrender_method_name(int m) { return m == TR_VIRGL ? "virgl" : "cells"; }
uint32_t termrender_generation(void) { return g_gen; }
termrender_stats_t* termrender_stats(void) { return &g_stats; }

void termrender_set_gpu(int on) {
    load();
    g_gpu = !!on;
    g_gen++;
    settings_set("term_gpu", on ? "on" : "off");
}
void termrender_set_method(int m) {
    load();
    g_method = m == TR_VIRGL ? TR_VIRGL : TR_CELLS;
    g_gen++;
    settings_set("term_renderer", termrender_method_name(g_method));
}

/* ── the VirGL renderer ──
 * An atlas texture holds each cell's picture (a glyph in its colours,
 * selected or not, with the cursor or not) once; a frame copies tiles from
 * it into a screen-sized texture with RESOURCE_COPY_REGION, then reads the
 * rectangle it changed back. */
#define OWNER      0x7ffffff0                      /* (the kernel's own: apps get their own ids) */
#define ATLAS_W    1024
#define ATLAS_H    1024
#define SLOTS      4096                            /* the tile table (open addressing; a power of 2) */
#define CMD_WORDS  4096

#define VIRGL_CCMD_RESOURCE_COPY_REGION 17
#define VIRGL_CCMD_SET_SUB_CTX          28
#define VIRGL_CCMD_CREATE_SUB_CTX       29
#define VIRGL_CMD0(cmd, obj, len) ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))
#define PIPE_TEXTURE_2D 2
#define VIRGL_FORMAT_B8G8R8X8_UNORM 2
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW  (1u << 3)

static int      g_failed;                          /* a call failed: software from now on */
static const char* g_why = "not started";
static uint32_t g_ctx, g_atlas, g_target;
static int      g_tw, g_th;                        /* the target's size (the screen's) */
static int      g_cw, g_ch, g_per_row, g_nslots, g_next_slot;
static uint64_t g_key[SLOTS];
static int16_t  g_slot[SLOTS];                     /* -1: empty */
static uint32_t g_cmd[CMD_WORDS];
static int      g_ncmd;

static int call(unsigned op, banana_gpu_request_t* q, void* data, unsigned size) {
    int rc = gpu_virgl_call(OWNER, op, q, sizeof(*q), data, size);
    if (rc < 0 && op != BGPU_RESOURCE_DESTROY) { g_failed = 1; g_why = "a VirGL call failed - using software"; klog("termrender: virgl op %u failed\n", op); }
    return rc;
}

static uint32_t make_texture(int w, int h) {
    banana_gpu_request_t q = {0};
    q.context = g_ctx;
    q.p[0] = PIPE_TEXTURE_2D;
    q.p[1] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    q.p[2] = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW;
    q.p[3] = (unsigned)w;
    q.p[4] = (unsigned)h;
    q.p[5] = 1;                                    /* depth */
    q.p[6] = 1;                                    /* array size */
    q.p[10] = (unsigned)(w * h * 4);
    int id = call(BGPU_RESOURCE_CREATE, &q, NULL, 0);
    if (id <= 0) return 0;
    q.resource = (unsigned)id;
    if (call(BGPU_ATTACH, &q, NULL, 0) < 0) return 0;
    return (uint32_t)id;
}

static int submit(void) {
    if (!g_ncmd) return 0;
    banana_gpu_request_t q = {0};
    q.context = g_ctx;
    int rc = call(BGPU_SUBMIT, &q, g_cmd, (unsigned)g_ncmd * 4);
    g_ncmd = 0;
    return rc;
}

int trv_available(void) {
    if (g_failed) return 0;
    if (!gpu_virgl_available()) { g_why = "no VirGL (virtio-gpu with 3D) - using software"; return 0; }
    if (g_ctx) return 1;
    banana_gpu_request_t q = {0};
    int c = call(BGPU_CONTEXT_CREATE, &q, NULL, 0);
    if (c <= 0) return 0;
    g_ctx = (uint32_t)c;
    g_cmd[0] = VIRGL_CMD0(VIRGL_CCMD_CREATE_SUB_CTX, 0, 1); g_cmd[1] = 0;
    g_cmd[2] = VIRGL_CMD0(VIRGL_CCMD_SET_SUB_CTX, 0, 1);    g_cmd[3] = 0;
    g_ncmd = 4;
    if (submit() < 0) return 0;
    if (!(g_atlas = make_texture(ATLAS_W, ATLAS_H))) return 0;
    g_why = "VirGL: tiles in a texture on the card";
    return 1;
}
const char* trv_status(void) { trv_available(); return g_why; }

static void forget_tiles(void) {
    for (int i = 0; i < SLOTS; i++) g_slot[i] = -1;
    g_next_slot = 0;
}

int trv_begin(int cw, int ch, int screen_w, int screen_h) {
    if (!trv_available()) return 0;
    if (cw != g_cw || ch != g_ch || !g_nslots) {        /* another cell size: the tiles drawn again */
        g_cw = cw;
        g_ch = ch;
        g_per_row = ATLAS_W / cw;
        g_nslots = g_per_row * (ATLAS_H / ch);
        if (g_nslots > SLOTS * 3 / 4) g_nslots = SLOTS * 3 / 4;
        forget_tiles();
    }
    if (screen_w != g_tw || screen_h != g_th) {
        if (g_target) {
            banana_gpu_request_t q = {0};
            q.context = g_ctx;
            q.resource = g_target;
            call(BGPU_RESOURCE_DESTROY, &q, NULL, 0);
        }
        g_target = make_texture(screen_w, screen_h);
        if (!g_target) return 0;
        g_tw = screen_w;
        g_th = screen_h;
    }
    g_ncmd = 0;
    return !g_failed;
}

static uint32_t hash(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33;
    return (uint32_t)k & (SLOTS - 1);
}
/* the tile's slot in the atlas, -1 */
static int find(uint64_t key) {
    for (uint32_t i = hash(key), n = 0; n < SLOTS; i = (i + 1) & (SLOTS - 1), n++) {
        if (g_slot[i] < 0) return -1;
        if (g_key[i] == key) return g_slot[i];
    }
    return -1;
}

int trv_has(uint64_t key) { return find(key) >= 0; }

int trv_upload(uint64_t key, const uint32_t* px, int stride) {
    if (g_failed) return 0;
    if (g_next_slot >= g_nslots) forget_tiles();       /* full: drawn again as they come */
    int s = g_next_slot++;
    int tx = (s % g_per_row) * g_cw, ty = (s / g_per_row) * g_ch;
    banana_gpu_request_t q = {0};
    q.context = g_ctx;
    q.resource = g_atlas;
    for (int r = 0; r < g_ch; r++) {
        q.offset = ((uint64_t)(ty + r) * ATLAS_W + (uint64_t)tx) * 4;
        if (call(BGPU_WRITE, &q, (void*)(px + r * stride), (unsigned)g_cw * 4) < 0) return 0;
    }
    q.offset = ((uint64_t)ty * ATLAS_W + (uint64_t)tx) * 4;
    q.p[0] = (unsigned)tx; q.p[1] = (unsigned)ty; q.p[2] = 0;
    q.p[3] = (unsigned)g_cw; q.p[4] = (unsigned)g_ch; q.p[5] = 1;
    q.p[7] = ATLAS_W * 4;
    if (call(BGPU_TRANSFER_PUT, &q, NULL, 0) < 0) return 0;
    uint32_t i = hash(key);
    while (g_slot[i] >= 0) i = (i + 1) & (SLOTS - 1);
    g_key[i] = key;
    g_slot[i] = (int16_t)s;
    return 1;
}

int trv_cell(uint64_t key, int x, int y) {
    int s = find(key);
    if (s < 0 || g_failed || x < 0 || y < 0 || x + g_cw > g_tw || y + g_ch > g_th) return 0;
    if (g_ncmd + 14 > CMD_WORDS && submit() < 0) return 0;
    uint32_t* c = g_cmd + g_ncmd;
    c[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, 13);
    c[1] = g_target; c[2] = 0; c[3] = (uint32_t)x; c[4] = (uint32_t)y; c[5] = 0;
    c[6] = g_atlas;  c[7] = 0; c[8] = (uint32_t)((s % g_per_row) * g_cw); c[9] = (uint32_t)((s / g_per_row) * g_ch); c[10] = 0;
    c[11] = (uint32_t)g_cw; c[12] = (uint32_t)g_ch; c[13] = 1;
    g_ncmd += 14;
    return 1;
}

int trv_finish(int x, int y, int w, int h, uint32_t* dst, int stride) {
    if (g_failed || submit() < 0) return 0;
    if (w <= 0 || h <= 0) return 1;
    if (x < 0 || y < 0 || x + w > g_tw || y + h > g_th) return 0;
    banana_gpu_request_t q = {0};
    q.context = g_ctx;
    q.resource = g_target;
    q.offset = ((uint64_t)y * (uint64_t)g_tw + (uint64_t)x) * 4;
    q.p[0] = (unsigned)x; q.p[1] = (unsigned)y; q.p[2] = 0;
    q.p[3] = (unsigned)w; q.p[4] = (unsigned)h; q.p[5] = 1;
    q.p[7] = (unsigned)g_tw * 4;
    if (call(BGPU_TRANSFER_GET, &q, NULL, 0) < 0) return 0;
    for (int r = 0; r < h; r++) {
        q.offset = ((uint64_t)(y + r) * (uint64_t)g_tw + (uint64_t)x) * 4;
        if (call(BGPU_READ, &q, dst + (y + r) * stride + x, (unsigned)w * 4) < 0) return 0;
    }
    return 1;
}

/* two tiles through the card: what comes back is what went in, where it was put */
void trv_tests(void (*report)(const char*, int, const char*)) {
    if (!trv_available()) return;                         /* (the caller says why: trv_status) */
    enum { W = 64, H = 32, CW = 8, CH = 16 };
    static uint32_t a[CW * CH], b[CW * CH], img[W * H];
    for (int i = 0; i < CW * CH; i++) { a[i] = 0x00102030u + (uint32_t)i * 0x010101u; b[i] = 0x00FF0000u ^ (uint32_t)i; }
    int ok = trv_begin(CW, CH, W, H);
    uint64_t ka = 0xA11CE000000001ull + (uint64_t)g_stats.full, kb = ka + 1;      /* (keys no frame uses) */
    ok = ok && trv_upload(ka, a, CW) && trv_upload(kb, b, CW) && trv_has(ka) && trv_has(kb);
    ok = ok && trv_cell(ka, 8, 0) && trv_cell(kb, 16, 16) && trv_cell(ka, 40, 16);
    for (int i = 0; i < W * H; i++) img[i] = 0x00ABCDEFu;
    ok = ok && trv_finish(0, 0, W, H, img, W);
    int same = ok;
    for (int y = 0; y < CH && same; y++)
        for (int x = 0; x < CW && same; x++)
            same = img[y * W + 8 + x] == a[y * CW + x] && img[(16 + y) * W + 16 + x] == b[y * CW + x] &&
                   img[(16 + y) * W + 40 + x] == a[y * CW + x];
    report("VirGL renderer: tiles copied on the card, read back exact", same, ok ? trv_status() : "a VirGL call failed");
    g_tw = g_th = 0;                                      /* (the next frame makes its screen-sized target again) */
    g_cw = 0;
}
