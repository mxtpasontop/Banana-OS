/* SPDX-License-Identifier: GPL-3.0-or-later */
/* (tools/tests/test_r8169.c: port I/O goes to the chip model) */
#ifndef IO_H
#define IO_H
#include "types.h"
uint8_t  fake_inb(uint16_t p);
uint16_t fake_inw(uint16_t p);
uint32_t fake_inl(uint16_t p);
void fake_outb(uint16_t p, uint8_t v);
void fake_outw(uint16_t p, uint16_t v);
void fake_outl(uint16_t p, uint32_t v);
static inline uint8_t  inb(uint16_t p) { return fake_inb(p); }
static inline uint16_t inw(uint16_t p) { return fake_inw(p); }
static inline uint32_t inl(uint16_t p) { return fake_inl(p); }
static inline void outb(uint16_t p, uint8_t v)  { fake_outb(p, v); }
static inline void outw(uint16_t p, uint16_t v) { fake_outw(p, v); }
static inline void outl(uint16_t p, uint32_t v) { fake_outl(p, v); }
static inline void io_wait(void) {}
#endif
