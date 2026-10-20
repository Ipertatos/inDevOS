/*
 * acpidump.c - dump the firmware's ACPI tables to files (Zeus `acpidump`).
 *
 * Writes every table the RSDT/XSDT lists, plus the DSDT and FACS that the
 * FADT points at, as raw binaries (decompile on a PC with `iasl -d`), and a
 * TABLES.TXT summary.  Also writes PCI.TXT (every PCI function with IDs,
 * BARs and interrupt line) and HINTS.TXT: a best-effort byte scan of the
 * DSDT/SSDTs for touchpad-related IDs (PNP0C50, AMDI0010/0019/0030, ...),
 * I2cSerialBus and GpioInt resource descriptors, and the Device each hit
 * sits in.  No AML is executed; the scan only reads bytes.
 *
 * Default output directory is /acpi on the root mount (the FAT32 partition
 * of the live USB stick when booted from it).  File names are 8.3.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "acpi.h"
#include "mem.h"
#include "vfs.h"
#include "pci.h"
#include "serial.h"
#include "stdio.h"

extern rsdt_t *rsdt;
extern xsdt_t *xsdt;
extern fadt_t *fadt;

#define AD_MAX_TABLES 128
#define AD_MAX_LEN    (16u << 20)

/* Return true if [va, va+len) is mapped in the current kernel page tables.
 * Handles the 1 GiB / 2 MiB pages the bootloader uses for the HHDM. */
static bool ad_mapped(uint64_t va, uint64_t len) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t end = va + len;
    uint64_t a = va & ~0xFFFull;
    while (a < end) {
        uint64_t *t = (uint64_t *)((cr3 & 0x000FFFFFFFFFF000ull) + vmm_higher_half_offset);
        uint64_t e = t[(a >> 39) & 511];
        if (!(e & 1)) return false;
        t = (uint64_t *)((e & 0x000FFFFFFFFFF000ull) + vmm_higher_half_offset);
        e = t[(a >> 30) & 511];
        if (!(e & 1)) return false;
        if (e & 0x80) { a = (a | ((1ull << 30) - 1)) + 1; continue; }
        t = (uint64_t *)((e & 0x000FFFFFFFFFF000ull) + vmm_higher_half_offset);
        e = t[(a >> 21) & 511];
        if (!(e & 1)) return false;
        if (e & 0x80) { a = (a | ((1ull << 21) - 1)) + 1; continue; }
        t = (uint64_t *)((e & 0x000FFFFFFFFFF000ull) + vmm_higher_half_offset);
        e = t[(a >> 12) & 511];
        if (!(e & 1)) return false;
        a += 0x1000;
    }
    return true;
}

static sdt_t *ad_table_at(uint64_t phys) {
    if (!phys) return NULL;
    uint64_t va = phys + vmm_higher_half_offset;
    if (!ad_mapped(va, sizeof(sdt_t))) return NULL;
    sdt_t *h = (sdt_t *)va;
    if (h->length < sizeof(sdt_t) || h->length > AD_MAX_LEN) return NULL;
    if (!ad_mapped(va, h->length)) return NULL;
    for (int i = 0; i < 4; i++) {
        uint8_t c = h->signature[i];
        if (c < 0x20 || c > 0x7E) return NULL;
    }
    return h;
}

static bool ad_checksum_ok(const sdt_t *h) {
    uint8_t s = 0;
    const uint8_t *p = (const uint8_t *)h;
    for (uint32_t i = 0; i < h->length; i++) s += p[i];
    return s == 0;
}

/* ---- growable text buffer ---- */
typedef struct { char *buf; uint32_t len, cap; } ad_text_t;

static void ad_puts(ad_text_t *t, const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    if (t->len + n + 1 > t->cap) {
        uint32_t nc = t->cap ? t->cap : 4096;
        while (t->len + n + 1 > nc) nc *= 2;
        char *nb = kalloc(nc);
        if (!nb) return;
        for (uint32_t i = 0; i < t->len; i++) nb[i] = t->buf[i];
        if (t->buf) kfree(t->buf);
        t->buf = nb; t->cap = nc;
    }
    for (uint32_t i = 0; i < n; i++) t->buf[t->len + i] = s[i];
    t->len += n;
    t->buf[t->len] = 0;
}

#define AD_PRINTF(t, ...) do { char _l[256]; snprintf(_l, sizeof(_l), __VA_ARGS__); ad_puts((t), _l); } while (0)

static void ad_str(char *out, const uint8_t *src, int n) {
    int i;
    for (i = 0; i < n; i++) {
        uint8_t c = src[i];
        out[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
    }
    out[i] = 0;
}

/* Left-justify s in a field of w characters (the kernel printf ignores '-'). */
static const char *ad_padr(char *buf, int bufsz, const char *s, int w) {
    int i = 0;
    while (s[i] && i < bufsz - 1) { buf[i] = s[i]; i++; }
    while (i < w && i < bufsz - 1) buf[i++] = ' ';
    buf[i] = 0;
    return buf;
}

/* ---- AML byte scan helpers (best effort; nothing is executed) ---- */

static int ad_pkglen_bytes(uint8_t lead) { return ((lead >> 6) & 3) + 1; }

static uint32_t ad_pkglen(const uint8_t *p) {
    int n = ad_pkglen_bytes(p[0]);
    if (n == 1) return p[0] & 0x3F;
    uint32_t v = p[0] & 0x0F;
    for (int i = 1; i < n; i++) v |= (uint32_t)p[i] << (4 + 8 * (i - 1));
    return v;
}

static bool ad_namechar(uint8_t c, bool lead) {
    if (c == '_' || (c >= 'A' && c <= 'Z')) return true;
    return !lead && c >= '0' && c <= '9';
}

/* Decode the NameString at p into out ("\_SB.PCI0.I2CA" style). */
static int ad_namestring(const uint8_t *p, const uint8_t *end, char *out, int outsz) {
    int o = 0;
    const uint8_t *q = p;
    while (q < end && (*q == '\\' || *q == '^') && o < outsz - 1) out[o++] = (char)*q++;
    int segs = 1;
    if (q < end && *q == 0x2E) { segs = 2; q++; }
    else if (q < end && *q == 0x2F) { if (q + 1 >= end) return -1; segs = q[1]; q += 2; }
    else if (q < end && *q == 0x00) { out[o] = 0; return (int)(q + 1 - p); }
    for (int s = 0; s < segs; s++) {
        if (q + 4 > end || !ad_namechar(q[0], true)) return -1;
        for (int i = 1; i < 4; i++) if (!ad_namechar(q[i], false)) return -1;
        if (s && o < outsz - 1) out[o++] = '.';
        for (int i = 0; i < 4 && o < outsz - 1; i++) out[o++] = (char)q[i];
        q += 4;
    }
    out[o] = 0;
    return (int)(q - p);
}

/* Innermost Device (5B 82) whose package covers offset off. */
static void ad_enclosing_device(const uint8_t *aml, uint32_t len, uint32_t off,
                                char *out, int outsz) {
    out[0] = '?'; out[1] = 0;
    uint32_t best_start = 0;
    bool found = false;
    uint32_t lo = off > (1u << 20) ? off - (1u << 20) : sizeof(sdt_t);
    for (uint32_t i = lo; i + 7 < off && i + 7 < len; i++) {
        if (aml[i] != 0x5B || aml[i + 1] != 0x82) continue;
        const uint8_t *pl = aml + i + 2;
        int nb = ad_pkglen_bytes(pl[0]);
        uint32_t plen = ad_pkglen(pl);
        if (plen < (uint32_t)nb + 4 || i + 2 + plen > len) continue;
        if (off < i + 2 || off >= i + 2 + plen) continue;
        char nm[64];
        if (ad_namestring(pl + nb, aml + len, nm, sizeof(nm)) < 0) continue;
        if (!found || i > best_start) {
            best_start = i;
            found = true;
            int k = 0;
            while (nm[k] && k < outsz - 1) { out[k] = nm[k]; k++; }
            out[k] = 0;
        }
    }
}

static bool ad_match(const uint8_t *a, uint32_t len, uint32_t i, const uint8_t *pat, uint32_t n) {
    if (i + n > len) return false;
    for (uint32_t k = 0; k < n; k++) if (a[i + k] != pat[k]) return false;
    return true;
}

static void ad_scan_aml(ad_text_t *t, const char *tname, const sdt_t *h) {
    const uint8_t *a = (const uint8_t *)h;
    uint32_t len = h->length;
    static const char *const ids[] = {
        "PNP0C50", "ACPI0C50", "AMDI0010", "AMDI0019", "AMDI0510", "AMDI0030",
        "AMD0030", "AMDI0040", "ELAN", "SYNA", "MSFT0001", "ALPS", "FTCS",
        "ATML", "CYAP", "PNP0C0D", "PNP0303", "PNP0F13", NULL
    };
    char dev[64], line[160];
    for (int k = 0; ids[k]; k++) {
        uint32_t n = 0;
        while (ids[k][n]) n++;
        for (uint32_t i = sizeof(sdt_t); i + n <= len; i++) {
            if (!ad_match(a, len, i, (const uint8_t *)ids[k], n)) continue;
            char ctx[24];
            uint32_t m = 0;
            while (m < 16 && i + m < len && a[i + m] >= 0x20 && a[i + m] <= 0x7E) m++;
            ad_str(ctx, a + i, (int)m);
            ad_enclosing_device(a, len, i, dev, sizeof(dev));
            snprintf(line, sizeof(line), "%s+0x%05x  id  \"%s\"  in Device %s\n", tname, i, ctx, dev);
            ad_puts(t, line);
            i += n - 1;
        }
    }
    /* EISAID("PNP0C50") compresses to 41 D0 0C 50 (DWordConst 0C prefix) */
    static const uint8_t eisa_c50[] = { 0x0C, 0x41, 0xD0, 0x0C, 0x50 };
    for (uint32_t i = sizeof(sdt_t); i + 5 <= len; i++) {
        if (!ad_match(a, len, i, eisa_c50, 5)) continue;
        ad_enclosing_device(a, len, i, dev, sizeof(dev));
        snprintf(line, sizeof(line), "%s+0x%05x  id  EISAID(PNP0C50)  in Device %s\n", tname, i, dev);
        ad_puts(t, line);
    }
    /* HID-over-I2C _DSM UUID 3CDFF6F7-4267-4555-AD05-B30A3D8938DE as ToUUID bytes */
    static const uint8_t hid_dsm[] = { 0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
                                       0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };
    for (uint32_t i = sizeof(sdt_t); i + 16 <= len; i++) {
        if (!ad_match(a, len, i, hid_dsm, 16)) continue;
        ad_enclosing_device(a, len, i, dev, sizeof(dev));
        snprintf(line, sizeof(line), "%s+0x%05x  dsm I2C-HID _DSM UUID  in Device %s\n", tname, i, dev);
        ad_puts(t, line);
    }
    /* I2cSerialBus(V2): 8E len16 rev srcidx type=1 gflags tflags16 trev tlen16 speed32 addr16 source */
    for (uint32_t i = sizeof(sdt_t); i + 18 <= len; i++) {
        if (a[i] != 0x8E || a[i + 5] != 1) continue;
        uint16_t dlen = (uint16_t)(a[i + 1] | (a[i + 2] << 8));
        uint8_t rev = a[i + 3];
        uint16_t tlen = (uint16_t)(a[i + 10] | (a[i + 11] << 8));
        uint32_t speed = (uint32_t)a[i + 12] | ((uint32_t)a[i + 13] << 8) |
                         ((uint32_t)a[i + 14] << 16) | ((uint32_t)a[i + 15] << 24);
        uint16_t addr = (uint16_t)(a[i + 16] | (a[i + 17] << 8));
        if (rev < 1 || rev > 2 || tlen < 6 || dlen < 15 || dlen > 200 || i + 3 + dlen > len) continue;
        if (speed < 10000 || speed > 5000000 || addr > 0x3FF) continue;
        char src[48];
        uint32_t so = i + 12 + tlen, m = 0;
        while (so + m < i + 3 + dlen && m < 40 && a[so + m] >= 0x20 && a[so + m] <= 0x7E) m++;
        ad_str(src, a + so, (int)m);
        ad_enclosing_device(a, len, i, dev, sizeof(dev));
        snprintf(line, sizeof(line),
                 "%s+0x%05x  res I2cSerialBus addr 0x%02x speed %u Hz %s controller \"%s\"  in Device %s\n",
                 tname, i, addr, speed, (a[i + 8] & 1) ? "10-bit" : "7-bit", src, dev);
        ad_puts(t, line);
    }
    /* GpioInt/GpioIo: 8C len16 rev type flags16 intflags16 pincfg drive16 debounce16
     *                 pintab16 srcidx srcname16 vend16 vendlen16 */
    for (uint32_t i = sizeof(sdt_t); i + 23 <= len; i++) {
        if (a[i] != 0x8C || a[i + 4] > 1) continue;
        uint16_t dlen = (uint16_t)(a[i + 1] | (a[i + 2] << 8));
        uint8_t rev = a[i + 3];
        uint16_t pint = (uint16_t)(a[i + 14] | (a[i + 15] << 8));
        uint16_t srcn = (uint16_t)(a[i + 17] | (a[i + 18] << 8));
        if (rev != 1 || dlen < 20 || dlen > 200 || i + 3 + dlen > len) continue;
        if (pint != 23 || srcn < pint + 2 || srcn >= dlen + 3) continue;
        uint16_t pin = (uint16_t)(a[i + pint] | (a[i + pint + 1] << 8));
        uint16_t iflags = (uint16_t)(a[i + 7] | (a[i + 8] << 8));
        char src[48];
        uint32_t m = 0;
        while (i + srcn + m < i + 3 + dlen && m < 40 && a[i + srcn + m] >= 0x20 && a[i + srcn + m] <= 0x7E) m++;
        ad_str(src, a + i + srcn, (int)m);
        ad_enclosing_device(a, len, i, dev, sizeof(dev));
        if (a[i + 4] == 0)
            snprintf(line, sizeof(line),
                     "%s+0x%05x  res GpioInt pin %u %s %s%s controller \"%s\"  in Device %s\n",
                     tname, i, pin, (iflags & 1) ? "edge" : "level",
                     ((iflags >> 1) & 3) == 0 ? "active-high" : ((iflags >> 1) & 3) == 1 ? "active-low" : "active-both",
                     (iflags & 0x10) ? " wake" : "", src, dev);
        else
            snprintf(line, sizeof(line), "%s+0x%05x  res GpioIo pin %u controller \"%s\"  in Device %s\n",
                     tname, i, pin, src, dev);
        ad_puts(t, line);
    }
}

static void ad_dump_pci(ad_text_t *t) {
    AD_PRINTF(t, "# bus:dev.fn vendor:device class.sub.progif irq_line irq_pin  BAR0..BAR5\n");
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *d = pci_devices[i];
        if (!d) continue;
        uint32_t intr = pci_device_read(d, 0x3C, 4);
        char line[256];
        int o = snprintf(line, sizeof(line), "%02x:%02x.%x %04x:%04x %02x.%02x.%02x irq %3u pin %u ",
                         d->bus, d->device, d->function, d->vendor_id, d->device_id,
                         d->class_code, d->subclass_code, d->prog_if,
                         intr & 0xFF, (intr >> 8) & 0xFF);
        if ((d->header_type & 0x7F) == 0) {
            for (int b = 0; b < 6 && o > 0 && o < (int)sizeof(line) - 12; b++)
                o += snprintf(line + o, sizeof(line) - (size_t)o, " %08x", pci_device_read(d, 0x10 + 4 * b, 4));
        }
        if (o > 0 && o < (int)sizeof(line) - 2) { line[o++] = '\n'; line[o] = 0; }
        ad_puts(t, line);
    }
}

static int ad_write(const char *dir, const char *name, const void *buf, uint32_t len) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int r = vfs_write_file(path, buf, len);
    if (r != 0) printf("acpidump: writing %s failed (%d)\n", path, r);
    return r;
}

void acpidump_command(int argc, char **argv) {
    const char *dir = "/acpi";
    bool hints_only = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'h') { hints_only = true; continue; }
        if (argv[i][0] == '/') { dir = argv[i]; continue; }
        printf("usage: acpidump [-h] [/output/dir]   (default /acpi)\n"
               "  writes DSDT.AML, SSDTn.AML, every other table, TABLES.TXT, PCI.TXT, HINTS.TXT\n"
               "  -h  only print the touchpad/I2C hints, write nothing\n");
        return;
    }
    if (!rsdt && !xsdt) { printf("acpidump: no RSDT/XSDT\n"); return; }
    if (!hints_only && !vfs_has_root_mount()) {
        printf("acpidump: nothing mounted at /; mount the USB stick or disk first (mount <dev>)\n");
        return;
    }

    static sdt_t *tabs[AD_MAX_TABLES];
    static uint64_t phys[AD_MAX_TABLES];
    int nt = 0;
    int entries = xsdt ? (int)((xsdt->header.length - sizeof(sdt_t)) / 8)
                       : (int)((rsdt->header.length - sizeof(sdt_t)) / 4);
    for (int i = 0; i < entries && nt < AD_MAX_TABLES - 2; i++) {
        uint64_t p = xsdt ? xsdt->tableptrs[i] : rsdt->tableptrs[i];
        sdt_t *h = ad_table_at(p);
        if (!h) { serial_printf("acpidump: entry %d at 0x%llx unreadable\n", i, (unsigned long long)p); continue; }
        tabs[nt] = h; phys[nt] = p; nt++;
    }
    if (fadt) {
        uint64_t dsdt = fadt->Dsdt, facs = fadt->FirmwareCtrl;
        if (fadt->header.length >= __builtin_offsetof(fadt_t, X_Dsdt) + sizeof(uint64_t) &&
            fadt->X_Dsdt && fadt->X_Dsdt < 0x0000800000000000ULL) dsdt = fadt->X_Dsdt;
        if (fadt->header.length >= __builtin_offsetof(fadt_t, X_FirmwareControl) + sizeof(uint64_t) &&
            fadt->X_FirmwareControl && fadt->X_FirmwareControl < 0x0000800000000000ULL) facs = fadt->X_FirmwareControl;
        sdt_t *h = ad_table_at(dsdt);
        if (h) { tabs[nt] = h; phys[nt] = dsdt; nt++; }
        /* FACS has a 4-byte signature and a length but no checksum/OEM fields */
        uint64_t fva = facs + vmm_higher_half_offset;
        if (facs && ad_mapped(fva, 8) && ((uint8_t *)fva)[0] == 'F' && ((uint8_t *)fva)[1] == 'A' &&
            ((uint8_t *)fva)[2] == 'C' && ((uint8_t *)fva)[3] == 'S' &&
            *(uint32_t *)(fva + 4) >= 64 && *(uint32_t *)(fva + 4) <= 4096 &&
            ad_mapped(fva, *(uint32_t *)(fva + 4))) {
            tabs[nt] = (sdt_t *)fva; phys[nt] = facs; nt++;
        }
    }

    ad_text_t list = {0}, hints = {0};
    AD_PRINTF(&list, "inDevOS acpidump: %d tables (%s)\n", nt, xsdt ? "XSDT" : "RSDT");
    AD_PRINTF(&list, "# file        sig     length rev phys               oem    oem_tbl   oem_rev  crtr     checksum\n");
    int ssdt_n = 0, written = 0;
    static char names[AD_MAX_TABLES][16];
    for (int i = 0; i < nt; i++) {
        sdt_t *h = tabs[i];
        char sig[5];
        ad_str(sig, h->signature, 4);
        for (int k = 0; k < 4; k++) if (sig[k] == ' ' || sig[k] == '/' || sig[k] == '.') sig[k] = '_';
        bool facs = sig[0] == 'F' && sig[1] == 'A' && sig[2] == 'C' && sig[3] == 'S';
        if (sig[0] == 'S' && sig[1] == 'S' && sig[2] == 'D' && sig[3] == 'T') {
            snprintf(names[i], sizeof(names[i]), "SSDT%d.AML", ++ssdt_n);
        } else {
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (tabs[j]->signature[0] == h->signature[0] && tabs[j]->signature[1] == h->signature[1] &&
                    tabs[j]->signature[2] == h->signature[2] && tabs[j]->signature[3] == h->signature[3]) dup++;
            if (dup) snprintf(names[i], sizeof(names[i]), "%s%d.AML", sig, dup + 1);
            else snprintf(names[i], sizeof(names[i]), "%s.AML", sig);
        }
        uint32_t len = facs ? *(uint32_t *)((uint8_t *)h + 4) : h->length;
        if (facs) {
            char pn[24];
            AD_PRINTF(&list, "%s  %s  %8u  -   0x%016llx\n", ad_padr(pn, sizeof(pn), names[i], 12),
                      sig, len, (unsigned long long)phys[i]);
        } else {
            char oem[8], otid[10], crt[6];
            ad_str(oem, h->oem_id, 6); ad_str(otid, h->oem_table_id, 8);
            ad_str(crt, (const uint8_t *)&h->creator_id, 4);
            char pn[24], po[12], pt[12], pc[8];
            AD_PRINTF(&list, "%s  %s  %8u  %2u  0x%016llx %s %s  %08x %s     %s\n",
                      ad_padr(pn, sizeof(pn), names[i], 12), sig, len, h->revision,
                      (unsigned long long)phys[i], ad_padr(po, sizeof(po), oem, 6),
                      ad_padr(pt, sizeof(pt), otid, 8), h->oem_revision,
                      ad_padr(pc, sizeof(pc), crt, 4), ad_checksum_ok(h) ? "ok" : "BAD");
        }
        if (!hints_only && ad_write(dir, names[i], h, len) == 0) written++;
        bool aml = (sig[0] == 'D' && sig[1] == 'S' && sig[2] == 'D' && sig[3] == 'T') ||
                   (sig[0] == 'S' && sig[1] == 'S' && sig[2] == 'D' && sig[3] == 'T');
        if (aml) {
            char tn[12];
            int k = 0;
            while (names[i][k] && names[i][k] != '.' && k < 11) { tn[k] = names[i][k]; k++; }
            tn[k] = 0;
            ad_scan_aml(&hints, tn, h);
        }
    }
    if (!hints.buf) ad_puts(&hints, "(no touchpad / I2C / GPIO hints found)\n");

    if (hints_only) {
        printf("%s", list.buf ? list.buf : "");
        printf("--- hints ---\n%s", hints.buf);
    } else {
        ad_text_t pci = {0};
        ad_dump_pci(&pci);
        if (list.buf && ad_write(dir, "TABLES.TXT", list.buf, list.len) == 0) written++;
        if (ad_write(dir, "HINTS.TXT", hints.buf, hints.len) == 0) written++;
        if (pci.buf && ad_write(dir, "PCI.TXT", pci.buf, pci.len) == 0) written++;
        if (pci.buf) kfree(pci.buf);
        printf("acpidump: %d tables, %d files written to %s\n", nt, written, dir);
        printf("--- hints (also in %s/HINTS.TXT) ---\n%s", dir, hints.buf);
        printf("On a PC: iasl -d DSDT.AML SSDT*.AML   (or: iasl -e SSDT*.AML -d DSDT.AML)\n");
        serial_printf("acpidump: %d tables, %d files written to %s\n", nt, written, dir);
        serial_printf("acpidump hints:\n%s", hints.buf);
    }
    if (list.buf) kfree(list.buf);
    if (hints.buf) kfree(hints.buf);
}
