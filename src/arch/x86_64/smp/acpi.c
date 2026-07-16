// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <miniOS/arch/x86_64/smp.h>
#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/io.h>

/* ------------------------------------------------------------------ */
/*  Globals declared in smp.h                                          */
/* ------------------------------------------------------------------ */

uint32_t smp_cpu_count = 0;
uint8_t  smp_lapic_ids[MAX_CPUS] = {0};

/* ------------------------------------------------------------------ */
/*  ACPI structure definitions (freestanding — no libc headers)       */
/* ------------------------------------------------------------------ */

/* RSDP v1 (20 bytes) */
struct acpi_rsdp {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0=v1, 2=v2 */
    uint32_t rsdt_phys;      /* physical addr of RSDT */
} __attribute__((packed));

/* RSDP v2 extension (36 bytes total) */
struct acpi_rsdp_v2 {
    struct acpi_rsdp v1;
    uint32_t length;
    uint64_t xsdt_phys;      /* physical addr of XSDT (64-bit) */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

/* SDT header (common to all ACPI tables) */
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* MADT (Multiple APIC Description Table), immediately follows acpi_sdt_header */
struct acpi_madt {
    struct acpi_sdt_header hdr;
    uint32_t lapic_phys;     /* physical address of LAPIC (unused — we use IA32_APIC_BASE) */
    uint32_t flags;          /* bit 0: PCAT_COMPAT (dual 8259 present) */
    /* followed by variable-length entries */
} __attribute__((packed));

/* MADT entry header */
struct madt_entry_hdr {
    uint8_t  type;
    uint8_t  length;
} __attribute__((packed));

/* MADT type 0: Processor Local APIC */
struct madt_lapic_entry {
    struct madt_entry_hdr hdr;  /* type=0, length=8 */
    uint8_t  acpi_proc_id;
    uint8_t  apic_id;
    uint32_t flags;             /* bit 0: enabled, bit 1: online-capable */
} __attribute__((packed));

#define MADT_TYPE_LAPIC               0
#define MADT_LAPIC_FLAG_ENABLED       (1 << 0)
#define MADT_LAPIC_FLAG_ONLINE_CAPABLE (1 << 1)

/* ------------------------------------------------------------------ */
/*  Helper: compare 8-byte signature                                  */
/* ------------------------------------------------------------------ */

static int sig8_match(const char *a, const char *b) {
    for (int i = 0; i < 8; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int sig4_match(const char *a, const char *b) {
    for (int i = 0; i < 4; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  MADT walker                                                        */
/* ------------------------------------------------------------------ */

static void parse_madt(struct acpi_madt *madt) {
    uint8_t *entry = (uint8_t *)madt + sizeof(struct acpi_madt);
    uint8_t *end   = (uint8_t *)madt + madt->hdr.length;

    while (entry < end) {
        struct madt_entry_hdr *h = (struct madt_entry_hdr *)entry;
        if (h->length == 0) break; /* guard: malformed table */

        if (h->type == MADT_TYPE_LAPIC) {
            struct madt_lapic_entry *e = (struct madt_lapic_entry *)entry;
            if ((e->flags & MADT_LAPIC_FLAG_ENABLED) ||
                (e->flags & MADT_LAPIC_FLAG_ONLINE_CAPABLE)) {
                if (smp_cpu_count < MAX_CPUS)
                    smp_lapic_ids[smp_cpu_count++] = e->apic_id;
            }
        }
        entry += h->length;
    }
}

/* ------------------------------------------------------------------ */
/*  RSDT walker (32-bit pointers)                                     */
/* ------------------------------------------------------------------ */

static struct acpi_madt *find_madt_in_rsdt(uint32_t rsdt_phys) {
    struct acpi_sdt_header *rsdt =
        (struct acpi_sdt_header *)(uint64_t)(rsdt_phys + KERNEL_VMA);

    uint32_t n_entries = (rsdt->length - sizeof(struct acpi_sdt_header)) / 4;
    uint32_t *entries  = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_sdt_header));

    for (uint32_t i = 0; i < n_entries; i++) {
        struct acpi_sdt_header *sdt =
            (struct acpi_sdt_header *)(uint64_t)(entries[i] + KERNEL_VMA);
        if (sig4_match(sdt->signature, "APIC")) {
            return (struct acpi_madt *)sdt;
        }
    }
    return (void *)0;
}

/* ------------------------------------------------------------------ */
/*  XSDT walker (64-bit pointers)                                     */
/* ------------------------------------------------------------------ */

static struct acpi_madt *find_madt_in_xsdt(uint64_t xsdt_phys) {
    struct acpi_sdt_header *xsdt =
        (struct acpi_sdt_header *)(xsdt_phys + KERNEL_VMA);

    uint32_t n_entries = (xsdt->length - sizeof(struct acpi_sdt_header)) / 8;
    uint64_t *entries  = (uint64_t *)((uint8_t *)xsdt + sizeof(struct acpi_sdt_header));

    for (uint32_t i = 0; i < n_entries; i++) {
        struct acpi_sdt_header *sdt =
            (struct acpi_sdt_header *)(entries[i] + KERNEL_VMA);
        if (sig4_match(sdt->signature, "APIC")) {
            return (struct acpi_madt *)sdt;
        }
    }
    return (void *)0;
}

/* ------------------------------------------------------------------ */
/*  RSDP -> MADT                                                      */
/* ------------------------------------------------------------------ */

static struct acpi_madt *madt_from_rsdp(struct acpi_rsdp *rsdp) {
    /* Prefer XSDT if revision >= 2 */
    if (rsdp->revision >= 2) {
        struct acpi_rsdp_v2 *rsdp2 = (struct acpi_rsdp_v2 *)rsdp;
        if (rsdp2->xsdt_phys != 0) {
            struct acpi_madt *m = find_madt_in_xsdt(rsdp2->xsdt_phys);
            if (m) return m;
        }
    }
    /* Fall back to RSDT */
    if (rsdp->rsdt_phys != 0) {
        return find_madt_in_rsdt(rsdp->rsdt_phys);
    }
    return (void *)0;
}

/* ------------------------------------------------------------------ */
/*  acpi_find_madt() — public entry point                             */
/* ------------------------------------------------------------------ */

void acpi_find_madt(uint64_t mb_info_phys) {
    struct acpi_rsdp *rsdp = (void *)0;

    /* ---- Phase A: scan Multiboot2 tags -------------------------------- */
    uint8_t *mb = (uint8_t *)(mb_info_phys + KERNEL_VMA);
    /* mb_info layout: uint32 total_size, uint32 reserved, then tags */
    uint32_t mb_total = *(uint32_t *)mb;
    uint8_t *tag = mb + 8;
    uint8_t *mb_end = mb + mb_total;

    while (tag < mb_end) {
        uint32_t tag_type = *(uint32_t *)tag;
        uint32_t tag_size = *(uint32_t *)(tag + 4);

        if (tag_type == 0) break; /* terminator tag */

        /* Tag 14 = ACPI 2.0 RSDP, Tag 15 = ACPI 1.0 RSDP */
        if (tag_type == 14 || tag_type == 15) {
            rsdp = (struct acpi_rsdp *)(tag + 8);
            break;
        }

        /* Tags are 8-byte aligned */
        uint32_t next_off = (tag_size + 7) & ~7u;
        tag += next_off;
    }

    /* ---- Phase B: memory scan fallback (EBDA + BIOS ROM) ------------- */
    if (!rsdp) {
        /* Scan EBDA: read 2-byte segment at physical 0x40E */
        uint16_t ebda_seg = *(volatile uint16_t *)(uint64_t)(0x40E + KERNEL_VMA);
        uint64_t ebda_phys = (uint64_t)ebda_seg << 4;
        uint8_t *ebda = (uint8_t *)(ebda_phys + KERNEL_VMA);

        for (uint32_t off = 0; off < 1024 && !rsdp; off += 16) {
            if (sig8_match((char *)(ebda + off), "RSD PTR ")) {
                rsdp = (struct acpi_rsdp *)(ebda + off);
            }
        }

        /* Scan BIOS ROM: 0xE0000..0xFFFFF */
        if (!rsdp) {
            uint8_t *bios = (uint8_t *)(uint64_t)(0xE0000 + KERNEL_VMA);
            for (uint32_t off = 0; off < 0x20000 && !rsdp; off += 16) {
                if (sig8_match((char *)(bios + off), "RSD PTR ")) {
                    rsdp = (struct acpi_rsdp *)(bios + off);
                }
            }
        }
    }

    if (!rsdp) {
        printk("ACPI: RSDP not found — assuming 1 CPU (BSP only)\n");
        /* Fallback: record BSP LAPIC ID */
        smp_lapic_ids[0] = (uint8_t)(lapic_read(LAPIC_ID) >> 24);
        smp_cpu_count = 1;
        return;
    }

    struct acpi_madt *madt = madt_from_rsdp(rsdp);
    if (!madt) {
        printk("ACPI: MADT not found — assuming 1 CPU (BSP only)\n");
        smp_lapic_ids[0] = (uint8_t)(lapic_read(LAPIC_ID) >> 24);
        smp_cpu_count = 1;
        return;
    }

    /* Walk MADT entries and collect LAPIC IDs */
    parse_madt(madt);

    if (smp_cpu_count == 0) {
        /* Degenerate MADT: fall back to BSP-only */
        smp_lapic_ids[0] = (uint8_t)(lapic_read(LAPIC_ID) >> 24);
        smp_cpu_count = 1;
    }

    /* ---- BSP reordering: ensure index 0 is the BSP ------------------- */
    uint8_t bsp_id = (uint8_t)(lapic_read(LAPIC_ID) >> 24);
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        if (smp_lapic_ids[i] == bsp_id) {
            /* Swap with index 0 */
            uint8_t tmp = smp_lapic_ids[0];
            smp_lapic_ids[0] = smp_lapic_ids[i];
            smp_lapic_ids[i] = tmp;
            break;
        }
    }

    printk("ACPI: found %u CPUs: LAPIC IDs", smp_cpu_count);
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        printk(" %u", smp_lapic_ids[i]);
    }
    printk("\n");
}
