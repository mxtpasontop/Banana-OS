/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "devices.h"
#include "pci.h"
#include "kstring.h"
#include "kheap.h"
#include "sysinfo.h"
#include "smp.h"
#include "gpu.h"
#include "fb.h"
#include "audio.h"
#include "disk.h"
#include "ata.h"
#include "blockdev.h"
#include "usb.h"
#include "serial.h"
#include "i2chid.h"
#include "gamepad.h"
#include "vmtools.h"
#include "applesmc.h"
#include "qga.h"
#include "virtio_serial.h"
#include "fs.h"
#include "../usb/usbcore.h"
#include "../net/net.h"
#include "../net/netdev.h"

static const char* const CATS[DC_COUNT] = {
    "Computer", "Disk drives", "Display adapters", "DVD/CD-ROM drives", "Human Interface Devices",
    "IDE ATA/ATAPI controllers", "Keyboards", "Mice and other pointing devices", "Monitors",
    "Network adapters", "Other devices", "Ports (COM & LPT)", "Processors",
    "Sound, video and game controllers", "Storage controllers", "System devices",
    "Universal Serial Bus controllers", "Universal Serial Bus devices",
};
const char* device_category_name(int cat) { return cat >= 0 && cat < DC_COUNT ? CATS[cat] : ""; }

const char* device_status_text(const device_t* d) {
    if (d->status == DS_NODRIVER) return "No driver is installed for this device.";
    return "This device is working properly.";
}

/* ── names ── */

static const struct { uint16_t id; const char* name; } VENDORS[] = {
    { 0x8086, "Intel Corporation" }, { 0x1022, "Advanced Micro Devices, Inc. [AMD]" },
    { 0x1002, "Advanced Micro Devices, Inc. [AMD/ATI]" }, { 0x10DE, "NVIDIA Corporation" },
    { 0x10EC, "Realtek Semiconductor Co., Ltd." }, { 0x1AF4, "Red Hat, Inc. (virtio)" },
    { 0x1B36, "Red Hat, Inc. (QEMU)" }, { 0x1234, "QEMU / Bochs" }, { 0x15AD, "VMware" },
    { 0x80EE, "Oracle VirtualBox" }, { 0x14E4, "Broadcom Inc." }, { 0x168C, "Qualcomm Atheros" },
    { 0x1969, "Qualcomm Atheros" }, { 0x1033, "NEC Corporation" }, { 0x1106, "VIA Technologies, Inc." },
    { 0x1B21, "ASMedia Technology Inc." }, { 0x144D, "Samsung Electronics Co Ltd" },
    { 0x15B7, "Sandisk Corp / Western Digital" }, { 0x1C5C, "SK hynix" }, { 0x1179, "Toshiba Corporation" },
    { 0x126F, "Silicon Motion, Inc." }, { 0x1987, "Phison Electronics Corporation" }, { 0x2646, "Kingston Technology" },
    { 0x1217, "O2 Micro, Inc." }, { 0x1180, "Ricoh Co Ltd" }, { 0x104C, "Texas Instruments" },
    { 0x11AB, "Marvell Technology Group Ltd." }, { 0x1B4B, "Marvell Technology Group Ltd." },
    { 0x1912, "Renesas Technology Corp." }, { 0x1414, "Microsoft Corporation (Hyper-V)" },
    { 0x5853, "XenSource, Inc." }, { 0x106B, "Apple Inc." }, { 0x1274, "Ensoniq" }, { 0x1102, "Creative Labs" },
    { 0x13F6, "C-Media Electronics Inc" }, { 0x8087, "Intel Corporation" }, { 0x0E8D, "MediaTek Inc." },
    { 0x14C3, "MediaTek Inc." }, { 0x17CB, "Qualcomm" }, { 0x1814, "Ralink corp." }, { 0x10B5, "PLX Technology, Inc." },
};
const char* pci_vendor_name(uint16_t v) {
    for (unsigned i = 0; i < sizeof(VENDORS) / sizeof(VENDORS[0]); i++) if (VENDORS[i].id == v) return VENDORS[i].name;
    return NULL;
}

/* the chips one meets most: in the virtual machines, and Intel's chipsets */
static const struct { uint16_t v, d; const char* name; } NAMES[] = {
    { 0x8086, 0x1237, "440FX - 82441FX PMC [Natoma] host bridge" },
    { 0x8086, 0x7000, "82371SB PIIX3 ISA bridge" }, { 0x8086, 0x7010, "82371SB PIIX3 IDE controller" },
    { 0x8086, 0x7020, "82371SB PIIX3 USB controller (UHCI)" }, { 0x8086, 0x7113, "82371AB/EB/MB PIIX4 ACPI" },
    { 0x8086, 0x7111, "82371AB/EB/MB PIIX4 IDE controller" },
    { 0x8086, 0x29C0, "82G33/G31/P35/P31 Express DRAM controller (Q35)" }, { 0x8086, 0x2918, "82801IB (ICH9) LPC interface controller" },
    { 0x8086, 0x2922, "82801IR/IO/IH (ICH9R) 6-port SATA controller [AHCI]" }, { 0x8086, 0x2930, "82801I (ICH9) SMBus controller" },
    { 0x8086, 0x2934, "82801I (ICH9) USB UHCI controller #1" }, { 0x8086, 0x293A, "82801I (ICH9) USB2 EHCI controller #1" },
    { 0x8086, 0x293E, "82801I (ICH9) HD Audio controller" }, { 0x8086, 0x2668, "82801FB/FBM/FR/FW/FRW (ICH6) HD Audio controller" },
    { 0x8086, 0x24CD, "82801DB/DBM (ICH4) USB2 EHCI controller" }, { 0x8086, 0x2415, "82801AA AC'97 Audio controller" },
    { 0x8086, 0x100E, "82540EM Gigabit Ethernet controller" }, { 0x8086, 0x100F, "82545EM Gigabit Ethernet controller" },
    { 0x8086, 0x10D3, "82574L Gigabit network connection" }, { 0x8086, 0x153A, "Ethernet connection I217-LM" },
    { 0x8086, 0x15B8, "Ethernet connection (2) I219-V" }, { 0x8086, 0x10EA, "82577LM Gigabit network connection" },
    { 0x8086, 0x1E31, "7 Series / C210 Series USB xHCI host controller" }, { 0x8086, 0x8C31, "8 Series / C220 Series USB xHCI host controller" },
    { 0x8086, 0xA12F, "100 Series / C230 Series USB 3.0 xHCI controller" }, { 0x8086, 0x9D2F, "Sunrise Point-LP USB 3.0 xHCI controller" },
    { 0x8086, 0x1E03, "7 Series chipset family 6-port SATA controller [AHCI mode]" }, { 0x8086, 0x8C02, "8 Series / C220 Series SATA controller [AHCI mode]" },
    { 0x8086, 0x1E20, "7 Series / C216 chipset family HD Audio controller" }, { 0x8086, 0x8C20, "8 Series / C220 Series HD Audio controller" },
    { 0x8086, 0xA170, "100 Series / C230 Series HD Audio controller" }, { 0x8086, 0x9D70, "Sunrise Point-LP HD Audio" },
    { 0x1234, 0x1111, "QEMU standard VGA (Bochs)" }, { 0x15AD, 0x0405, "SVGA II adapter" },
    { 0x15AD, 0x0740, "Virtual Machine Communication Interface" }, { 0x15AD, 0x07A0, "PCI Express root port" },
    { 0x80EE, 0xBEEF, "VirtualBox graphics adapter" }, { 0x80EE, 0xCAFE, "VirtualBox guest service" },
    { 0x1AF4, 0x1000, "Virtio network device" }, { 0x1AF4, 0x1001, "Virtio block device" },
    { 0x1AF4, 0x1002, "Virtio memory balloon" }, { 0x1AF4, 0x1003, "Virtio console" },
    { 0x1AF4, 0x1004, "Virtio SCSI" }, { 0x1AF4, 0x1005, "Virtio RNG" },
    { 0x1AF4, 0x1041, "Virtio 1.0 network device" }, { 0x1AF4, 0x1042, "Virtio 1.0 block device" },
    { 0x1AF4, 0x1043, "Virtio 1.0 console" }, { 0x1AF4, 0x1044, "Virtio 1.0 RNG" },
    { 0x1AF4, 0x1045, "Virtio 1.0 memory balloon" }, { 0x1AF4, 0x1048, "Virtio 1.0 SCSI" },
    { 0x1AF4, 0x1050, "Virtio 1.0 GPU" }, { 0x1AF4, 0x1052, "Virtio 1.0 input" },
    { 0x1B36, 0x0001, "QEMU PCI-PCI bridge" }, { 0x1B36, 0x0004, "QEMU PCI expander bridge" },
    { 0x1B36, 0x000C, "QEMU PCIe root port" }, { 0x1B36, 0x000D, "QEMU xHCI host controller" },
    { 0x1B36, 0x0010, "QEMU NVM Express controller" }, { 0x1B36, 0x0005, "QEMU PCI test device" }, { 0x1B36, 0x0100, "QXL paravirtual graphic card" },
    { 0x10EC, 0x8139, "RTL-8100/8101L/8139 PCI Fast Ethernet adapter" }, { 0x10EC, 0x8168, "RTL8111/8168/8411 PCI Express Gigabit Ethernet controller" },
    { 0x10EC, 0x8169, "RTL8169 PCI Gigabit Ethernet controller" }, { 0x10EC, 0x8136, "RTL810xE PCI Express Fast Ethernet controller" },
    { 0x1022, 0x2000, "79c970 [PCnet32 LANCE]" }, { 0x1274, 0x5000, "ES1370 [AudioPCI]" },
    { 0x1414, 0x5353, "Hyper-V virtual VGA" },
};

const char* pci_class_name(uint8_t c, uint8_t s, uint8_t pi) {
    switch (c) {
    case 0x00: return s == 0x01 ? "VGA-compatible device" : "Unclassified device";
    case 0x01:
        switch (s) {
        case 0x00: return "SCSI storage controller";
        case 0x01: return "IDE interface";
        case 0x04: return "RAID bus controller";
        case 0x05: return "ATA controller";
        case 0x06: return pi == 0x01 ? "SATA controller (AHCI)" : "SATA controller";
        case 0x07: return "Serial Attached SCSI controller";
        case 0x08: return "Non-Volatile memory controller (NVMe)";
        }
        return "Mass storage controller";
    case 0x02: return s == 0x80 ? "Network controller" : s == 0x00 ? "Ethernet controller" : "Network controller";
    case 0x03: return s == 0x00 ? "VGA compatible controller" : s == 0x02 ? "3D controller" : "Display controller";
    case 0x04: return s == 0x03 ? "Audio device (HD Audio)" : s == 0x01 ? "Multimedia audio controller" : s == 0x00 ? "Multimedia video controller" : "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        case 0x07: return "CardBus bridge";
        case 0x80: return "Bridge";
        }
        return "Bridge";
    case 0x07: return s == 0x00 ? "Serial controller" : s == 0x01 ? "Parallel controller" : "Communication controller";
    case 0x08:
        switch (s) {
        case 0x00: return "PIC";
        case 0x01: return "DMA controller";
        case 0x02: return "Timer";
        case 0x03: return "RTC";
        case 0x05: return "SD Host controller";
        case 0x06: return "IOMMU";
        }
        return "System peripheral";
    case 0x09: return "Input device controller";
    case 0x0C:
        switch (s) {
        case 0x00: return "FireWire (IEEE 1394)";
        case 0x03:
            switch (pi) {
            case 0x00: return "USB controller (UHCI)";
            case 0x10: return "USB controller (OHCI)";
            case 0x20: return "USB controller (EHCI)";
            case 0x30: return "USB controller (xHCI)";
            }
            return "USB controller";
        case 0x05: return "SMBus";
        case 0x80: return "Serial bus controller";
        }
        return "Serial bus controller";
    case 0x0D: return "Wireless controller";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing controller";
    case 0x12: return "Processing accelerator";
    case 0x13: return "Non-essential instrumentation";
    case 0xFF: return "Unassigned class";
    }
    return "Device";
}

void pci_device_name(const void* pv, char* out, int cap) {
    const pci_dev_t* d = (const pci_dev_t*)pv;
    for (unsigned i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (NAMES[i].v == d->vendor && NAMES[i].d == d->device) { ksnprintf(out, (size_t)cap, "%s", NAMES[i].name); return; }
    if (d->class_code == 0x03) {
        const char* fam = gpu_family_name(d->vendor, d->device);
        if (fam) { ksnprintf(out, (size_t)cap, "%s", fam); return; }
    }
    ksnprintf(out, (size_t)cap, "%s", pci_class_name(d->class_code, d->subclass, d->prog_if));
}

/* ── the scan ── */

typedef struct { device_t* out; int n, max; } scan_t;

static device_t* add(scan_t* s, int cat, int kind, const char* name) {
    if (s->n >= s->max) return NULL;
    device_t* d = &s->out[s->n++];
    memset(d, 0, sizeof(*d));
    d->cat = cat;
    d->kind = kind;
    d->parent = -1;
    d->slot = -1;
    ksnprintf(d->name, sizeof(d->name), "%s", name);
    return d;
}

static void res(device_t* d, const char* fmt, const char* a) {
    if (d->nres < DEV_RES) ksnprintf(d->res[d->nres++], sizeof(d->res[0]), fmt, a);
}

static void set_driver(device_t* d, const char* drv) {
    ksnprintf(d->driver, sizeof(d->driver), "%s", drv ? drv : "");
    d->status = drv && drv[0] ? DS_OK : DS_NODRIVER;
}

/* virtio's shared PCI code takes the device for the driver of its kind */
static const char* virtio_driver(uint16_t dev) {
    switch (dev) {
    case 0x1000: case 0x1041: return "virtio_net";
    case 0x1001: case 0x1042: return "virtio_blk";
    case 0x1004: case 0x1048: return "virtio_scsi";
    case 0x1003: case 0x1043: return "virtio_serial";
    case 0x1050:              return "gpu_virtio";
    }
    return "virtio_pci";
}

static int pci_cat(const pci_dev_t* p) {
    switch (p->class_code) {
    case 0x01: return p->subclass == 0x01 || p->subclass == 0x05 || p->subclass == 0x06 ? DC_IDE : DC_STORAGE;
    case 0x02: case 0x0D: return DC_NETWORK;
    case 0x03: return DC_DISPLAY;
    case 0x04: return DC_SOUND;
    case 0x07: return p->subclass <= 0x01 ? DC_PORTS : DC_SYSTEM;
    case 0x08: return p->subclass == 0x05 ? DC_STORAGE : DC_SYSTEM;
    case 0x09: return DC_HID;
    case 0x0C: return p->subclass == 0x03 ? DC_USB : DC_SYSTEM;
    case 0x05: case 0x06: case 0x0B: case 0x10: case 0x11: case 0x12: return DC_SYSTEM;
    }
    return DC_OTHER;
}

static int scan_pci_cb(const pci_dev_t* p, void* ctx) {
    scan_t* s = (scan_t*)ctx;
    char name[64];
    pci_device_name(p, name, sizeof(name));
    device_t* d = add(s, pci_cat(p), DK_PCI, name);
    if (!d) return 1;
    d->bus = p->bus; d->dev = p->dev; d->fn = p->fn;
    const char* v = pci_vendor_name(p->vendor);
    if (v) ksnprintf(d->maker, sizeof(d->maker), "%s", v);
    else ksnprintf(d->maker, sizeof(d->maker), "Vendor %04x", p->vendor);
    ksnprintf(d->loc, sizeof(d->loc), "PCI bus %d, device %d, function %d", p->bus, p->dev, p->fn);
    ksnprintf(d->hwid, sizeof(d->hwid), "PCI\\VEN_%04X&DEV_%04X&CC_%02X%02X%02X", p->vendor, p->device,
              p->class_code, p->subclass, p->prog_if);
    const char* drv = pci_driver_of(p->bus, p->dev, p->fn);
    if (drv && !strcmp(drv, "virtio_pci")) drv = virtio_driver(p->device);
    gpu_t* g = gpu_active();
    if (!drv && g && g->pci.bus == p->bus && g->pci.dev == p->dev && g->pci.fn == p->fn && g->pci.vendor == p->vendor) drv = g->driver;
    if (!drv && p->class_code == 0x01 && p->subclass == 0x01) drv = "ata";             /* (the legacy ports need no enabling) */
    set_driver(d, drv);
    if (!drv && p->class_code == 0x03) {
        /* a GPU without its own driver: the screen the firmware set up still works */
        ksnprintf(d->driver, sizeof(d->driver), "%s", sysinfo_get()->uefi ? "efifb" : "vesafb");
        d->status = DS_OK;
    }
    if (!drv && (p->class_code == 0x06 || p->class_code == 0x05 || (p->class_code == 0x08 && p->subclass != 0x05) ||
                 (p->class_code == 0x0C && p->subclass == 0x05))) {
        d->status = DS_BUILTIN;                   /* bridges, the chipset's parts: nothing to drive */
    }
    if (d->status == DS_NODRIVER) d->cat = DC_OTHER;
    /* resources */
    char b[48];
    if (p->irq_line && p->irq_line != 0xFF) { ksnprintf(b, sizeof(b), "IRQ %d", p->irq_line); res(d, "%s", b); }
    for (int i = 0; i < 6 && d->nres < DEV_RES; i++) {
        uint32_t raw = pci_read32(p->bus, p->dev, p->fn, (uint8_t)(0x10 + i * 4));
        if (!raw) continue;
        if (raw & 1) ksnprintf(b, sizeof(b), "I/O range %04x", raw & ~3u);
        else {
            uint64_t a = raw & ~15u;
            if (((raw >> 1) & 3) == 2 && i < 5) {
                a |= (uint64_t)pci_read32(p->bus, p->dev, p->fn, (uint8_t)(0x14 + i * 4)) << 32;
                i++;
            }
            if (!a) continue;
            if (a >> 32) ksnprintf(b, sizeof(b), "Memory range %x%08x%s", (uint32_t)(a >> 32), (uint32_t)a, raw & 8 ? " (prefetchable)" : "");
            else ksnprintf(b, sizeof(b), "Memory range %08x%s", (uint32_t)a, raw & 8 ? " (prefetchable)" : "");
        }
        res(d, "%s", b);
    }
    if (p->class_code == 0x06 && (p->subclass == 0x04 || p->subclass == 0x07))
        d->secondary = (uint8_t)(pci_read32(p->bus, p->dev, p->fn, 0x18) >> 8);
    return 0;
}

static int find_pci_driver(scan_t* s, const char* drv, int nth) {
    for (int i = 0; i < s->n; i++)
        if (s->out[i].kind == DK_PCI && !strcmp(s->out[i].driver, drv) && nth-- == 0) return i;
    return -1;
}

static const char* usb_class_name(const usb_info_t* u) {
    switch (u->cls) {
    case 0x01: return "USB audio device";
    case 0x02: return "USB communications device";
    case 0x03: return u->kbd ? "USB keyboard" : u->mouse ? "USB mouse" : "USB input device";
    case 0x06: return "USB camera (still image)";
    case 0x07: return "USB printer";
    case 0x08: return "USB mass storage device";
    case 0x09: return "USB hub";
    case 0x0A: return "USB CDC data";
    case 0x0B: return "USB smart card reader";
    case 0x0E: return "USB video device";
    case 0xE0: return "Bluetooth adapter";
    case 0xEF: return "USB composite device";
    }
    return "USB device";
}

static void scan_usb(scan_t* s) {
    int first = s->n;
    int slot_of[USB_MAX_DEVICES];
    for (int i = 0; i < USB_MAX_DEVICES; i++) slot_of[i] = -1;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_info_t u;
        if (!usb_info(i, &u)) continue;
        char name[64];
        const char* drv = u.driver;
        int cat, kind = DK_USB;
        if (u.pad || (drv && !strcmp(drv, "xpad")))      { cat = DC_HID; kind = DK_PAD; }
        else if (u.kbd)                                   { cat = DC_KEYBOARD; kind = DK_KBD; }
        else if (u.mouse)                                 { cat = DC_MICE; kind = DK_MOUSE; }
        else if (u.cls == 0x03)                           cat = DC_HID;
        else if (u.cls == 0x01)                           cat = DC_SOUND;
        else if (u.cls == 0x09 || u.cls == 0x08)          cat = DC_USB;
        else if (drv && (!strcmp(drv, "r8152") || !strcmp(drv, "cdc_ecm") || !strcmp(drv, "usbnet"))) cat = DC_NETWORK;
        else if (u.cls == 0xE0)                           cat = DC_NETWORK;
        else                                              cat = DC_USBDEV;
        if (u.product && u.product[0]) ksnprintf(name, sizeof(name), "%s", u.product);
        else ksnprintf(name, sizeof(name), "%s", usb_class_name(&u));
        if (!drv && cat != DC_USB) cat = DC_OTHER;
        device_t* d = add(s, cat, kind, name);
        if (!d) return;
        slot_of[i] = s->n - 1;
        d->slot = i;
        set_driver(d, drv);
        if (u.cls == 0x09 && !drv) set_driver(d, "usb-hub");
        if (u.manufacturer && u.manufacturer[0]) ksnprintf(d->maker, sizeof(d->maker), "%s", u.manufacturer);
        else ksnprintf(d->maker, sizeof(d->maker), "Vendor %04x", u.vid);
        if (u.parent >= 0) ksnprintf(d->loc, sizeof(d->loc), "Port %d of a hub on USB bus %d", u.port, u.bus + 1);
        else ksnprintf(d->loc, sizeof(d->loc), "Port %d on USB bus %d", u.port, u.bus + 1);
        ksnprintf(d->hwid, sizeof(d->hwid), "USB\\VID_%04X&PID_%04X", u.vid, u.pid);
        char b[48];
        ksnprintf(b, sizeof(b), "USB %x.%x, %s", u.bcd_usb >> 8, (u.bcd_usb >> 4) & 15, u.speed);
        res(d, "%s", b);
        ksnprintf(b, sizeof(b), "Class %02x, subclass %02x, protocol %02x", u.cls, u.sub, u.proto);
        res(d, "%s", b);
        ksnprintf(b, sizeof(b), "%s", usb_class_name(&u));
        if (strcmp(b, name)) res(d, "Kind: %s", b);
        /* by connection: under its hub, or its host controller */
        d->parent = -(u.bus + 2);                 /* (resolved below) */
        d->secondary = (uint8_t)(u.parent >= 0 && u.parent < USB_MAX_DEVICES && u.parent < 255 ? u.parent : 0xFF);
    }
    for (int k = first; k < s->n; k++) {
        device_t* d = &s->out[k];
        if (d->secondary != 0xFF && slot_of[d->secondary] >= 0) d->parent = slot_of[d->secondary];
        else {
            int bus = -d->parent - 2, nth = 0;
            const char* hn = usb_hc_name(bus);
            for (int h = 0; h < bus; h++) if (!strcmp(usb_hc_name(h), hn)) nth++;
            d->parent = find_pci_driver(s, hn, nth);
        }
        d->secondary = 0;
    }
}

static void size_text(uint64_t bytes, char* out, int cap) {
    if (bytes >= (uint64_t)1 << 40) ksnprintf(out, (size_t)cap, "%u.%u TB", (uint32_t)(bytes >> 40), (uint32_t)((bytes >> 30) & 1023) * 10 / 1024);
    else if (bytes >= (uint64_t)1 << 30) ksnprintf(out, (size_t)cap, "%u.%u GB", (uint32_t)(bytes >> 30), (uint32_t)((bytes >> 20) & 1023) * 10 / 1024);
    else ksnprintf(out, (size_t)cap, "%u MB", (uint32_t)(bytes >> 20));
}

static void scan_disks(scan_t* s) {
    ata_disk_t* all = (ata_disk_t*)kmalloc(sizeof(ata_disk_t) * DISK_MAX);
    if (!all) return;
    int n = disk_probe(all, DISK_MAX);
    blockdev_t* seen[DISK_MAX];
    int nseen = 0;
    for (int i = 0; i < n; i++) {
        const ata_disk_t* a = &all[i];
        char name[64], sz[16], where[48];
        disk_describe(a, where, sizeof(where));
        size_text((uint64_t)a->sectors * 512u, sz, sizeof(sz));
        if (a->is_atapi) ksnprintf(name, sizeof(name), "%s", a->model[0] ? a->model : "CD drive");
        else ksnprintf(name, sizeof(name), "%s (%s)", a->model[0] ? a->model : "Disk", sz);
        device_t* d = add(s, a->is_atapi ? DC_CDROM : DC_DISK, a->is_atapi ? DK_CDROM : DK_DISK, name);
        if (!d) break;
        const char* drv = a->nvme ? "nvme" : a->vblk ? "virtio_blk" : a->scsi ? "virtio_scsi" : a->mmc ? "sdhci" :
                          a->ahci_port >= 0 ? "ahci" : "ata";
        set_driver(d, drv);
        ksnprintf(d->loc, sizeof(d->loc), "%s", where);
        ksnprintf(d->maker, sizeof(d->maker), "%s", "(Standard disk drives)");
        if (!a->is_atapi) {
            char b[48];
            ksnprintf(b, sizeof(b), "%u sectors of 512 bytes", a->sectors);
            res(d, "%s", b);
        }
        d->parent = find_pci_driver(s, drv, 0);
        blockdev_t* bd = disk_blockdev(a);
        if (bd) { ksnprintf(d->key, sizeof(d->key), "%s", bd->name); if (nseen < DISK_MAX) seen[nseen++] = bd; }
        if (bd && bd->system) res(d, "%s", "Holds Banana OS (the system disk)");
    }
    kfree(all);
    int nusb = 0;
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* bd = blockdev_get(i);
        if (!bd || !bd->present) continue;
        int dup = 0;
        for (int k = 0; k < nseen; k++) if (seen[k] == bd) dup = 1;
        if (dup) continue;
        char name[64], sz[16];
        size_text((uint64_t)bd->sectors * (bd->sector_size ? bd->sector_size : 512), sz, sizeof(sz));
        ksnprintf(name, sizeof(name), "%s (%s)", bd->model[0] ? bd->model : bd->name, sz);
        device_t* d = add(s, DC_DISK, DK_BLOCK, name);
        if (!d) return;
        int usb = !strcmp(bd->kind, "usb");
        set_driver(d, usb ? "usb-storage" : bd->kind);
        ksnprintf(d->key, sizeof(d->key), "%s", bd->name);
        ksnprintf(d->maker, sizeof(d->maker), "%s", "(Standard disk drives)");
        ksnprintf(d->loc, sizeof(d->loc), "%s (%s)", bd->name, usb ? "removable" : bd->kind);
        if (bd->nvol) res(d, "Mounted at %s", fs_mount_point(bd->mnt));
        /* by connection: the n-th USB mass storage device */
        if (usb) {
            int k = nusb++;
            for (int j = 0; j < s->n; j++)
                if (s->out[j].kind == DK_USB && !strcmp(s->out[j].driver, "usb-storage") && k-- == 0) { d->parent = j; break; }
        }
    }
}

int devices_scan(device_t* out, int max) {
    scan_t s = { out, 0, max };
    const sysinfo_t* si = sysinfo_get();
    /* the computer */
#ifdef __x86_64__
    device_t* d = add(&s, DC_COMPUTER, DK_OTHER, "ACPI x64-based PC");
#else
    device_t* d = add(&s, DC_COMPUTER, DK_OTHER, "ACPI x86-based PC");
#endif
    if (d) {
        set_driver(d, "kernel");
        res(d, "Firmware: %s", si->uefi ? "UEFI" : "BIOS");
        const char* hv = vmtools_hypervisor();
        if (hv && hv[0]) res(d, "Virtual machine: %s", hv);
    }
    /* the processors */
    for (int i = 0; i < cpu_count(); i++) {
        d = add(&s, DC_PROCESSOR, DK_CPU, si->cpu_brand[0] ? si->cpu_brand : si->cpu_vendor);
        if (!d) break;
        set_driver(d, "smp");
        ksnprintf(d->maker, sizeof(d->maker), "%s", si->cpu_vendor);
        char b[48];
        ksnprintf(b, sizeof(b), "Processor %d", i);
        ksnprintf(d->loc, sizeof(d->loc), "%s", b);
        ksnprintf(b, sizeof(b), "Family %u, model %u, stepping %u", si->cpu_family, si->cpu_model, si->cpu_stepping);
        res(d, "%s", b);
    }
    /* the PCI functions */
    pci_scan(scan_pci_cb, &s);
    /* bridges: each function under the bridge to its bus */
    for (int i = 0; i < s.n; i++) {
        if (s.out[i].kind != DK_PCI || !s.out[i].bus) continue;
        for (int j = 0; j < s.n; j++)
            if (s.out[j].kind == DK_PCI && s.out[j].secondary == s.out[i].bus) { s.out[i].parent = j; break; }
    }
    scan_usb(&s);
    scan_disks(&s);
    /* the keyboard and the mouse */
    if (ps2_controller_present()) {
        d = add(&s, DC_KEYBOARD, DK_KBD, "Standard PS/2 keyboard");
        if (d) { set_driver(d, "keyboard"); ksnprintf(d->loc, sizeof(d->loc), "%s", "PS/2 port (i8042)"); ksnprintf(d->maker, sizeof(d->maker), "%s", "(Standard keyboards)"); }
        char tp[64];
        tp[0] = 0;
        if (mouse_is_touchpad()) touchpad_describe(tp, sizeof(tp));
        d = add(&s, DC_MICE, DK_MOUSE, tp[0] ? tp : "PS/2 compatible mouse");
        if (d) { set_driver(d, "mouse"); ksnprintf(d->loc, sizeof(d->loc), "%s", "PS/2 port (i8042)"); ksnprintf(d->maker, sizeof(d->maker), "%s", "(Standard mouse types)"); }
    }
    {
        char ih[64];
        if (i2chid_describe(ih, sizeof(ih)) > 0 && ih[0]) {
            d = add(&s, DC_MICE, DK_MOUSE, ih);
            if (d) { set_driver(d, "i2chid"); ksnprintf(d->loc, sizeof(d->loc), "%s", "I2C bus"); }
        }
    }
    for (int i = 0; i < GP_MAX; i++) {
        gamepad_t g;
        if (!gamepad_get(i, &g)) continue;
        int dup = 0;
        for (int k = 0; k < s.n; k++) if (s.out[k].kind == DK_PAD) dup = 1;
        if (dup) break;
        d = add(&s, DC_HID, DK_PAD, g.name[0] ? g.name : "Game controller");
        if (d) set_driver(d, "gamepad");
    }
    /* the screen */
    const fb_info_t* fi = fb_info();
    if (fi && fi->width) {
        char b[64];
        ksnprintf(b, sizeof(b), "Generic monitor (%ux%u)", fi->width, fi->height);
        d = add(&s, DC_MONITOR, DK_MONITOR, b);
        if (d) {
            gpu_t* g = gpu_active();
            set_driver(d, g ? g->driver : (si->uefi ? "efifb" : "vesafb"));
            ksnprintf(d->maker, sizeof(d->maker), "%s", "(Standard monitor types)");
            ksnprintf(b, sizeof(b), "%ux%u, %u bits per pixel", fi->width, fi->height, fi->bpp);
            res(d, "%s", b);
            for (int j = 0; j < s.n; j++) if (s.out[j].cat == DC_DISPLAY && s.out[j].kind == DK_PCI) { d->parent = j; break; }
        }
    }
    /* the ports and the firmware's devices */
    if (serial_present()) {
        d = add(&s, DC_PORTS, DK_OTHER, "Communications port (COM1)");
        if (d) { set_driver(d, "serial"); res(d, "%s", "I/O range 03f8-03ff"); res(d, "%s", "IRQ 4"); }
    }
    static const char* const SYS[] = { "ACPI system", "Programmable interrupt controller (APIC)", "System timer", "System CMOS / real time clock", "Direct memory access controller" };
    static const char* const SYSDRV[] = { "acpi", "smp", "timer", "rtc", "kernel" };
    for (int i = 0; i < 5; i++) {
        d = add(&s, DC_SYSTEM, DK_OTHER, SYS[i]);
        if (d) { set_driver(d, SYSDRV[i]); ksnprintf(d->maker, sizeof(d->maker), "%s", "(Standard system devices)"); }
    }
    if (applesmc_present()) { d = add(&s, DC_SYSTEM, DK_OTHER, "Apple System Management Controller"); if (d) set_driver(d, "applesmc"); }
    if (qga_active()) {
        char b[64];
        qga_describe(b, sizeof(b));
        d = add(&s, DC_SYSTEM, DK_OTHER, "QEMU guest agent");
        if (d) { set_driver(d, "qga"); res(d, "%s", b); }
    }
    if (audio_available()) {
        /* the sound card's name, on its PCI function */
        for (int j = 0; j < s.n; j++)
            if (s.out[j].cat == DC_SOUND && s.out[j].kind == DK_PCI && s.out[j].status == DS_OK) { res(&s.out[j], "Playing as: %s", audio_device_name()); break; }
    }
    /* network adapters: their interface names */
    for (int i = 0; i < net_device_count(); i++) {
        netdev_t* nd = net_device_at(i);
        if (!nd) continue;
        for (int j = 0; j < s.n; j++) {
            device_t* e = &s.out[j];
            if (e->cat != DC_NETWORK || strcmp(e->driver, nd->name) || e->nres >= DEV_RES) continue;
            int done = 0;
            for (int r = 0; r < e->nres; r++) if (!strncmp(e->res[r], "Interface ", 10)) done = 1;
            if (done) continue;
            char b[48];
            ksnprintf(b, sizeof(b), "Interface %s, MAC %02x:%02x:%02x:%02x:%02x:%02x", nd->ifname,
                      nd->mac[0], nd->mac[1], nd->mac[2], nd->mac[3], nd->mac[4], nd->mac[5]);
            res(e, "%s", b);
            break;
        }
    }
    return s.n;
}
