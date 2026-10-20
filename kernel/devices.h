/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DEVICES_H
#define DEVICES_H

#include "types.h"

/*
 * Every device Banana OS knows of, as Device Manager shows them (the window:
 * kernel/devmgr.c; the shell: `devmgr`, `lspci`): the PCI functions with the
 * driver that took each one (kernel/pci.h), the USB devices, the drives,
 * the processors, the keyboard and mouse, the screen, the ports and the
 * firmware's devices - by type, or by connection (each under the bridge,
 * controller or hub it hangs off).
 */

enum {                               /* the types, in Device Manager's order */
    DC_COMPUTER = 0, DC_DISK, DC_DISPLAY, DC_CDROM, DC_HID, DC_IDE, DC_KEYBOARD, DC_MICE,
    DC_MONITOR, DC_NETWORK, DC_OTHER, DC_PORTS, DC_PROCESSOR, DC_SOUND, DC_STORAGE,
    DC_SYSTEM, DC_USB, DC_USBDEV, DC_COUNT
};
enum { DS_OK = 0, DS_BUILTIN, DS_NODRIVER };     /* a driver; needs none; has none */
enum { DK_PCI = 0, DK_USB, DK_DISK, DK_CDROM, DK_BLOCK, DK_CPU, DK_KBD, DK_MOUSE, DK_MONITOR, DK_PAD, DK_OTHER };

#define DEV_MAX  128
#define DEV_RES  6

typedef struct {
    int     cat;                     /* DC_* */
    int     status;                  /* DS_* */
    int     kind;                    /* DK_* */
    char    name[64];
    char    driver[24];              /* "" without one */
    char    maker[40];
    char    loc[64];                 /* "PCI bus 0, device 3, function 0" */
    char    hwid[48];                /* "PCI\VEN_8086&DEV_100E&CC_020000" */
    char    res[DEV_RES][48];        /* resources and more facts */
    int     nres;
    int     parent;                  /* by connection: the device it hangs off, -1 the computer */
    char    key[16];                 /* a drive's name ("usb0"), for its actions */
    uint8_t bus, dev, fn, secondary; /* PCI: where; a bridge: the bus behind it */
    int     slot;                    /* USB: its slot (usb_info) */
} device_t;

/* fills out (up to max) and returns how many */
int         devices_scan(device_t* out, int max);
const char* device_category_name(int cat);   /* English: wrap in _() to show */
const char* device_status_text(const device_t* d);
/* "Intel Corporation", NULL */
const char* pci_vendor_name(uint16_t vendor);
/* "SATA controller (AHCI)" */
const char* pci_class_name(uint8_t cls, uint8_t sub, uint8_t prog_if);
/* the name devices_scan gives a PCI function */
void        pci_device_name(const void* pci_dev, char* out, int cap);

#endif
