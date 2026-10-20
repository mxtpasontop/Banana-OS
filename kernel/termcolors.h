/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TERMCOLORS_H
#define TERMCOLORS_H

#include "types.h"

/*
 * Terminal windows' colour schemes (kernel/termcolors.c): what the 16
 * colours of the text - VGA's order, the background 0 - look like, as in
 * the terminals of other systems (Windows Terminal's Campbell, Solarized,
 * Dracula...). One for all the terminal windows, kept in
 * /etc/settings.conf as term_colors; the full-screen console keeps VGA's.
 */

int         termcolors_count(void);
const char* termcolors_name(int i);
/* the scheme by name (any case, '-' / '_' / ' ' the same): its index, -1 */
int         termcolors_find(const char* name);
int         termcolors_current(void);
void        termcolors_set(int i);            /* saved for the next boot */
/* colour c (0-15) of the scheme in use */
uint32_t    termcolors_rgb(uint8_t c);
uint32_t    termcolors_scheme_rgb(int scheme, uint8_t c);
/* bumped by every change: for redrawing */
uint32_t    termcolors_generation(void);

#endif
