/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SHELL_H
#define SHELL_H

void shell_run(void);

/* Runs a full shell instance (its own prompt, history, commands - "help",
 * "top", "edit", etc. all work exactly as in the main console) bound to a
 * single GUI terminal window's virtual terminal `vt`. Never returns
 * (loops for the lifetime of the OS, like shell_run()); meant to be run
 * as its own cooperative task (see kernel/task.h) so each open terminal
 * window makes independent progress instead of sharing one shell's
 * output with whichever window last had focus. */
void shell_run_window(int vt);

/* The shell of an SSH session on remote terminal `tty` (kernel/tty.h):
 * waits for a client, serves it until it logs out, repeats. Never returns. */
void shell_run_remote(int tty);

/* the desktop's Start > Shut down: saves the files to the installed disk,
 * then powers off (0) or restarts (1); does not return */
void shell_power(int restart);
void shell_sleep(void);

/* (selftest) line as Tab completes it at its end: 1 if it changed */
int shell_complete(const char* line, char* out, int cap);
/* (selftest) runs one command line, as if typed at a prompt */
void shell_exec(const char* line);

#endif
