/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * languages: an app that speaks the desktop's language (Settings >
 * Language) - its texts written in English and wrapped in _(), French, German,
 * Italian and Romansh catalogs given once. A text with no translation stays in English;
 * on a Banana OS without languages everything is English.
 *     languages            the greeting, in the desktop's language
 *     languages FILE.lang  ... with the translations of a .lang file too
 */
#define BANANA_UNDERSCORE
#include <banana.h>
#include <stdio.h>

static const banana_tr_t FR[] = {
    { "Hello!", "Bonjour !" },
    { "This app speaks the desktop's language: %s.", "Cette application parle la langue du bureau : %s." },
    { "Change it in Settings > Language, or with: language fr", "Changez-la dans Paramètres > Langue, ou avec : language fr" },
    { "Goodbye.", "Au revoir." },
    { 0, 0 },
};
static const banana_tr_t DE[] = {
    { "Hello!", "Hallo!" },
    { "This app speaks the desktop's language: %s.", "Diese App spricht die Sprache des Desktops: %s." },
    { "Change it in Settings > Language, or with: language fr", "Ändern Sie sie in Einstellungen > Sprache, oder mit: language fr" },
    { "Goodbye.", "Auf Wiedersehen." },
    { 0, 0 },
};
static const banana_tr_t IT[] = {
    { "Hello!", "Ciao!" },
    { "This app speaks the desktop's language: %s.", "Questa app parla la lingua del desktop: %s." },
    { "Change it in Settings > Language, or with: language fr", "Cambiala in Impostazioni > Lingua, o con: language fr" },
    { "Goodbye.", "Arrivederci." },
    { 0, 0 },
};
static const banana_tr_t RM[] = {
    { "Hello!", "Allegra!" },
    { "This app speaks the desktop's language: %s.", "Questa app discurra la lingua dal desktop: %s." },
    { "Change it in Settings > Language, or with: language fr", "La mida en Configuraziun > Lingua, u cun: language fr" },
    { "Goodbye.", "A revair." },
    { 0, 0 },
};
static const banana_catalog_t LANGS[] = { { "fr", FR }, { "de", DE }, { "it", IT }, { "rm", RM }, { 0, 0 } };

int main(int argc, char** argv) {
    banana_tr_init(LANGS);
    if (argc > 1) {
        int n = banana_tr_load(argv[1]);
        printf("%s: %d\n", argv[1], n);
    }
    printf("%s\n", _("Hello!"));
    printf(_("This app speaks the desktop's language: %s."), banana_language());
    printf("\n%s\n", _("Change it in Settings > Language, or with: language fr"));
    printf("%s\n", _("Goodbye."));
    return 0;
}
