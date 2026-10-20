/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "i18n.h"
#include "kstring.h"
#include "settings.h"

extern const i18n_pair_t I18N_FR[];
extern const int I18N_FR_N;
extern const i18n_pair_t I18N_DE[];
extern const int I18N_DE_N;
extern const i18n_pair_t I18N_IT[];
extern const int I18N_IT_N;
extern const i18n_pair_t I18N_RM[];
extern const int I18N_RM_N;

static const struct { const char* code; const char* name; const i18n_pair_t* pairs; const int* n; } LANGS[] = {
    { "en", "English", 0, 0 },
    { "fr", "Fran\xC3\xA7" "ais", I18N_FR, &I18N_FR_N },
    { "de", "Deutsch", I18N_DE, &I18N_DE_N },
    { "it", "Italiano", I18N_IT, &I18N_IT_N },
    { "rm", "Rumantsch", I18N_RM, &I18N_RM_N },
};
#define NLANGS ((int)(sizeof(LANGS) / sizeof(LANGS[0])))

static int      g_cur;
static uint32_t g_gen = 1;

/* the catalog in use, by the English text's hash (open addressing) */
#define SLOTS 4096
static const i18n_pair_t* g_slot[SLOTS];

static uint32_t hash(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static void build(void) {
    memset(g_slot, 0, sizeof(g_slot));
    if (!LANGS[g_cur].pairs) return;
    int n = *LANGS[g_cur].n;
    for (int i = 0; i < n; i++) {
        const i18n_pair_t* p = &LANGS[g_cur].pairs[i];
        if (!p->en || !p->tr || !p->tr[0]) continue;
        uint32_t h = hash(p->en) & (SLOTS - 1);
        for (int k = 0; k < SLOTS && g_slot[h]; k++) h = (h + 1) & (SLOTS - 1);
        g_slot[h] = p;
    }
}

const char* tr(const char* en) {
    if (!en || g_cur == 0 || !en[0]) return en;
    uint32_t h = hash(en) & (SLOTS - 1);
    for (int k = 0; k < SLOTS && g_slot[h]; k++, h = (h + 1) & (SLOTS - 1))
        if (!strcmp(g_slot[h]->en, en)) return g_slot[h]->tr;
    return en;                                   /* (not translated yet: English) */
}

int i18n_use(const char* code) {
    for (int i = 0; i < NLANGS; i++)
        if (code && !strcmp(code, LANGS[i].code)) {
            if (i != g_cur) { g_cur = i; build(); g_gen++; }
            return 0;
        }
    return -1;
}

int i18n_set(const char* code) {
    if (i18n_use(code) < 0) return -1;
    settings_set("language", LANGS[g_cur].code);
    return 0;
}

void i18n_boot(void) {
    char v[8];
    if (settings_get("language", v, sizeof(v))) i18n_use(v);
}

const char* i18n_lang(void) { return LANGS[g_cur].code; }
int i18n_count(void) { return NLANGS; }
const char* i18n_code(int i) { return i >= 0 && i < NLANGS ? LANGS[i].code : ""; }
const char* i18n_name(int i) { return i >= 0 && i < NLANGS ? LANGS[i].name : ""; }
int i18n_current(void) { return g_cur; }
uint32_t i18n_generation(void) { return g_gen; }
