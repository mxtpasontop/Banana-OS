/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef I18N_H
#define I18N_H

#include "types.h"

/*
 * The desktop's language: English (the default), French, German, Italian or Romansh. Text
 * shown to the user is written in English and wrapped in _(): it comes back
 * in the language chosen, from that language's catalog (kernel/lang_*.c:
 * pairs of English and its translation), or as it is when the catalog has
 * no translation for it - so a text not translated yet still shows, in
 * English. The choice is kept in the settings ("language"), made by the
 * installer and in Settings > Language; apps read it with banana_language().
 */

typedef struct { const char* en; const char* tr; } i18n_pair_t;

const char* tr(const char* en);
#define _(s) tr(s)

void        i18n_boot(void);                   /* the language saved */
int         i18n_set(const char* code);        /* "en", "fr", "de", "it", "rm": 0, or -1 (unknown); saves it */
int         i18n_use(const char* code);        /* the same without saving (the installer's choice) */
const char* i18n_lang(void);                   /* "en" */
int         i18n_count(void);                  /* the languages there are */
const char* i18n_code(int i);                  /* "fr" */
const char* i18n_name(int i);                  /* "Français": in its own language */
int         i18n_current(void);                /* its index */
uint32_t    i18n_generation(void);             /* changes with the language: redraw */

#endif
