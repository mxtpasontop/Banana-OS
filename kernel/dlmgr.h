/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DLMGR_H
#define DLMGR_H

#include "types.h"
#include "fs.h"

/*
 * The Browser's downloads (kernel/dlmgr.c), like Chrome's: each runs in a
 * task of its own, so the tab goes on while it comes in, written into its
 * file as it arrives; the list - running, done, failed, cancelled - is
 * kept in ~/.config/downloads and shown on about:downloads.
 */

enum { DLM_QUEUED, DLM_RUNNING, DLM_DONE, DLM_FAILED, DLM_CANCELLED };

typedef struct {
    int      id, state;
    char     name[FS_NAME_LEN];
    char     path[FS_PATH_LEN];
    char     url[256];             /* (shortened for showing) */
    uint32_t got, total;           /* total 0: not known */
    uint32_t rate;                 /* bytes a second, while it runs */
    uint32_t when;                 /* rtc_now() it started */
    char     err[80];
} dlm_info_t;

/* the cookie header for a request to url (the Browser's jar), into out */
typedef void (*dlm_cookies_fn)(const char* url, char* out, int cap);
void dlm_set_cookies(dlm_cookies_fn f);

/* a download of url into path (its file, the name already free); its id, or -1 */
int  dlm_start(const char* url, const char* path);
/* the list, newest first: 1 if entry i is there */
int  dlm_get(int i, dlm_info_t* out);
int  dlm_find(int id, dlm_info_t* out);
void dlm_cancel(int id);
void dlm_retry(int id);
void dlm_remove(int id);          /* from the list (the file stays) */
void dlm_clear(void);             /* every finished one off the list */
/* how many run now, and their bytes so far / in all (0 when not known) */
int  dlm_running(uint32_t* got, uint32_t* total);
/* the first id of this start's downloads (the list's older ones are below) */
int  dlm_session_base(void);
/* changes whenever what the list shows changes */
uint32_t dlm_gen(void);

#endif
