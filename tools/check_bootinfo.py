#!/usr/bin/env python3
"""
tools/check_bootinfo.py — проверяет, что boot/bootinfo.h (C) и boot/bootinfo.inc
(NASM) описывают одну и ту же структуру pht_bootinfo_t / pht_imghdr_t.

Запускается Makefile'ом перед сборкой образа. Ничего не генерирует.
"""
import re
import sys

H = "boot/bootinfo.h"
INC = "boot/bootinfo.inc"

# поле структуры в .h  ->  константа в .inc
BOOTINFO_FIELDS = {
    "magic": "BI_MAGIC", "bios_drive": "BI_DRIVE", "media_type": "BI_MEDIA",
    "sector_shift": "BI_SHIFT", "flags": "BI_FLAGS", "image_lba": "BI_IMAGE_LBA",
    "kernel_addr": "BI_KERNEL_ADDR", "kernel_sectors": "BI_KERNEL_SECTORS",
    "ramdisk_addr": "BI_RAMDISK_ADDR", "ramdisk_sectors": "BI_RAMDISK_SECTORS",
    "image_sectors": "BI_IMAGE_SECTORS", "fs_lba": "BI_FS_LBA",
    "mem_total_kb": "BI_MEM_TOTAL_KB", "mbr": "BI_MBR",
}
IMGHDR_FIELDS = {
    "magic": "IH_MAGIC", "version": "IH_VERSION", "stage2_sectors": "IH_STAGE2_SECTORS",
    "init_lba": "IH_INIT_LBA", "init_sectors": "IH_INIT_SECTORS", "fs_lba": "IH_FS_LBA",
    "image_sectors": "IH_IMAGE_SECTORS", "kernel_addr": "IH_KERNEL_ADDR",
    "ramdisk_addr": "IH_RAMDISK_ADDR",
}
SIZES = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4}


def parse_struct(text, typedef_name):
    m = re.search(r"typedef struct __attribute__\(\(packed\)\) \{([^}]*)\} " + typedef_name,
                  text, re.S)
    if not m:
        sys.exit("check_bootinfo: не нашёл структуру %s в %s" % (typedef_name, H))
    offsets, off = {}, 0
    for line in m.group(1).splitlines():
        line = line.split("/*")[0].strip()
        fm = re.match(r"(uint8_t|uint16_t|uint32_t)\s+(\w+)(?:\[(\d+)\])?;", line)
        if not fm:
            continue
        t, name, count = fm.groups()
        offsets[name] = off
        off += SIZES[t] * (int(count) if count else 1)
    return offsets, off


def parse_inc(text):
    vals = {}
    for line in text.splitlines():
        m = re.match(r"\s*(\w+)\s+equ\s+([^;]+)", line)
        if m:
            expr = m.group(2).strip()
            try:
                vals[m.group(1)] = int(eval(expr, {}, dict(vals)))
            except Exception:
                pass
    return vals


def main():
    h = open(H, encoding="utf-8").read()
    inc = parse_inc(open(INC, encoding="utf-8").read())
    ok = True

    def check(c_name, inc_name, expected):
        nonlocal ok
        if inc.get(inc_name) != expected:
            print("check_bootinfo: %s: %s = %s, но в %s %s = %s"
                  % (H, c_name, hex(expected), INC, inc_name, inc.get(inc_name)))
            ok = False

    bi, bi_size = parse_struct(h, "pht_bootinfo_t")
    for f, c in BOOTINFO_FIELDS.items():
        check("pht_bootinfo_t." + f, c, bi[f])
    check("sizeof(pht_bootinfo_t)", "BI_SIZE", bi_size)

    ih, _ = parse_struct(h, "pht_imghdr_t")
    for f, c in IMGHDR_FIELDS.items():
        check("pht_imghdr_t." + f, c, ih[f])

    defines = dict(re.findall(r"#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?", h))
    check("PHT_BOOTINFO_ADDR", "BOOTINFO_ADDR", int(defines["PHT_BOOTINFO_ADDR"], 0))
    check("PHT_BOOTINFO_MAGIC", "BOOTINFO_MAGIC", int(defines["PHT_BOOTINFO_MAGIC"], 0))
    check("PHT_IMGHDR_OFFSET", "IMGHDR_OFFSET", int(defines["PHT_IMGHDR_OFFSET"], 0))
    check("PHT_IMGHDR_MAGIC", "IMGHDR_MAGIC", int(defines["PHT_IMGHDR_MAGIC"], 0))
    check("PHT_BIT_OFFSET", "BIT_OFFSET", int(defines["PHT_BIT_OFFSET"], 0))
    for name in ("UNKNOWN", "FLOPPY", "HDD", "CDROM"):
        check("PHT_MEDIA_" + name, "MEDIA_" + name, int(defines["PHT_MEDIA_" + name], 0))

    if not ok:
        sys.exit(1)
    print("  bootinfo.h / bootinfo.inc: OK (sizeof(pht_bootinfo_t) = 0x%X)" % bi_size)


if __name__ == "__main__":
    main()
