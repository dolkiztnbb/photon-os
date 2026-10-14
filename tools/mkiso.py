#!/usr/bin/env python3
"""
tools/mkiso.py — делает загрузочный ISO (CD-ROM) из сырого образа os-image.bin.

Не требует xorriso/genisoimage: ISO 9660 + El Torito собираются вручную.

Как это работает:
  * весь os-image.bin кладётся в ISO как обычный файл PHOTON.IMG и
    одновременно объявляется загрузочным образом El Torito в режиме
    «no emulation»: BIOS загрузит первые 4 виртуальных сектора (2 КБ) файла
    по адресу 0x7C00 и передаст управление нашему MBR (boot/bootsect.asm);
  * в байты 8..63 загрузочного файла записывается стандартная El Torito
    boot-info-table (как делает `mkisofs -boot-info-table`): из неё stage1
    узнаёт LBA образа внутри ISO (bi_file) и понимает, что грузится с ISO;
  * в system area (первые 32 КБ ISO) записывается тот же MBR с таблицей
    разделов, покрывающей ISO — получается «гибридный» ISO: его можно
    записать dd/Rufus (DD-режим) на флешку, и BIOS загрузит его как USB-HDD.
    В этом случае bi_file тоже != 0, а сектор 512 байт — stage1 пересчитает
    LBA образа как bi_file*4.

Использование:
  mkiso.py --image output/build/image/os-image.bin --out output/build/image/photon.iso
"""
import argparse
import os
import struct
import sys

BLOCK = 2048

PVD_BLOCK = 16
BOOTREC_BLOCK = 17
TERM_BLOCK = 18
CATALOG_BLOCK = 19
PATHTABLE_L_BLOCK = 20
PATHTABLE_M_BLOCK = 21
ROOTDIR_BLOCK = 22
FIRST_FILE_BLOCK = 23

BOOT_LOAD_SECTORS = 4          # 4 * 512 = 2048 байт грузит BIOS
BIT_OFFSET = 8                 # El Torito boot-info-table в загрузочном файле
MBR_PART_TABLE = 0x1BE


def die(msg):
    print("mkiso: ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def blocks(nbytes):
    return (nbytes + BLOCK - 1) // BLOCK


def pad_block(data):
    rem = len(data) % BLOCK
    return data + (b"\x00" * (BLOCK - rem) if rem else b"")


def a_str(s, n):
    return s.encode("ascii").ljust(n, b" ")[:n]


def dir_record(extent, size, ident, is_dir):
    rec = bytearray()
    rec += b"\x00"                       # длина записи (заполним ниже)
    rec += b"\x00"                       # extended attribute length
    rec += both32(extent)
    rec += both32(size)
    rec += bytes([100, 1, 1, 0, 0, 0, 0]) # дата записи: 2000-01-01 00:00:00 GMT
    rec += bytes([0x02 if is_dir else 0x00])
    rec += b"\x00\x00"                   # file unit size, interleave gap
    rec += both16(1)                     # volume sequence number
    rec += bytes([len(ident)])
    rec += ident
    if len(rec) % 2:
        rec += b"\x00"
    rec[0] = len(rec)
    return bytes(rec)


def path_table(root_block, little):
    fmt = "<I" if little else ">I"
    pfmt = "<H" if little else ">H"
    rec = bytes([1, 0]) + struct.pack(fmt, root_block) + struct.pack(pfmt, 1) + b"\x00\x00"
    return rec


def boot_catalog(image_block, load_sectors):
    # validation entry
    v = bytearray(32)
    v[0] = 0x01                   # header id
    v[1] = 0x00                   # platform: 80x86
    v[4:28] = b"PhotonOS".ljust(24, b"\x00")
    v[30] = 0x55
    v[31] = 0xAA
    words = struct.unpack("<16H", bytes(v))
    csum = (-sum(words)) & 0xFFFF
    struct.pack_into("<H", v, 28, csum)
    assert sum(struct.unpack("<16H", bytes(v))) & 0xFFFF == 0

    # initial/default entry: no emulation, load segment 0 (=0x7C0), N sectors
    e = struct.pack("<BBHBBHI", 0x88, 0x00, 0x0000, 0x00, 0x00, load_sectors, image_block)
    e += b"\x00" * 20
    cat = bytes(v) + e
    return pad_block(cat)


def boot_info_table(pvd_block, file_block, file_data):
    """56 байт: bi_pvd, bi_file, bi_length, bi_csum, 40 резерв."""
    body = file_data[64:]
    if len(body) % 4:
        body += b"\x00" * (4 - len(body) % 4)
    csum = sum(struct.unpack("<%dI" % (len(body) // 4), body)) & 0xFFFFFFFF
    return struct.pack("<IIII", pvd_block, file_block, len(file_data), csum) + b"\x00" * 40


def chs_bytes(lba, heads=64, spt=32):
    c = lba // (heads * spt)
    h = (lba // spt) % heads
    s = lba % spt + 1
    if c > 1023:
        return bytes([0xFE, 0xFF, 0xFF])
    return bytes([h, (s & 0x3F) | ((c >> 2) & 0xC0), c & 0xFF])


def make_hybrid_mbr(mbr, total_sectors_512):
    """MBR для system area: наш загрузчик + раздел на весь ISO (как isohybrid)."""
    m = bytearray(mbr)
    part = bytearray(64)
    part[0] = 0x80                       # активный
    part[1:4] = chs_bytes(0)
    part[4] = 0x17                       # тип (как у isohybrid)
    part[5:8] = chs_bytes(total_sectors_512 - 1)
    struct.pack_into("<II", part, 8, 0, total_sectors_512)
    m[MBR_PART_TABLE:MBR_PART_TABLE + 64] = part
    m[510] = 0x55
    m[511] = 0xAA
    return bytes(m)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True, help="сырой образ os-image.bin")
    ap.add_argument("--out", required=True, help="куда писать ISO")
    ap.add_argument("--volid", default="PHOTONOS")
    ap.add_argument("--no-hybrid", action="store_true",
                    help="не записывать MBR в system area (только CD)")
    ap.add_argument("--min-blocks", type=int, default=160,
                    help="минимальный размер ISO в блоках 2048 (некоторые приводы "
                         "не любят совсем крошечные диски)")
    args = ap.parse_args()

    image = bytearray(open(args.image, "rb").read())
    if len(image) < 512 or image[510:512] != b"\x55\xAA":
        die("%s не похож на образ с загрузочным сектором" % args.image)
    if len(image) % 2048:
        die("размер образа (%d) должен быть кратен 2048 — иначе BIOS не сможет "
            "читать его секторами CD" % len(image))
    if len(image) < BOOT_LOAD_SECTORS * 512:
        die("образ слишком мал")

    image_block = FIRST_FILE_BLOCK
    image_blocks = blocks(len(image))

    # --- boot-info-table в загрузочном файле ------------------------------------
    bit = boot_info_table(PVD_BLOCK, image_block, bytes(image))
    image[BIT_OFFSET:BIT_OFFSET + len(bit)] = bit

    # --- файлы в корне -------------------------------------------------------------
    files = [("PHOTON.IMG;1", image_block, len(image))]
    files.sort()

    # --- корневой каталог -----------------------------------------------------------
    root = b""
    root += dir_record(ROOTDIR_BLOCK, BLOCK, b"\x00", True)   # "."
    root += dir_record(ROOTDIR_BLOCK, BLOCK, b"\x01", True)   # ".."
    for name, block, size in files:
        root += dir_record(block, size, name.encode("ascii"), False)
    if len(root) > BLOCK:
        die("слишком много файлов для одного блока каталога")
    root = pad_block(root)

    # --- таблицы путей ---------------------------------------------------------------
    pt_l = path_table(ROOTDIR_BLOCK, True)
    pt_m = path_table(ROOTDIR_BLOCK, False)
    pt_size = len(pt_l)

    total_blocks = image_block + image_blocks
    if total_blocks < args.min_blocks:
        total_blocks = args.min_blocks
    total_blocks = (total_blocks + 15) // 16 * 16   # кратно 32 КБ

    # --- Primary Volume Descriptor ------------------------------------------------
    pvd = bytearray(BLOCK)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = a_str("PHOTONOS", 32)
    pvd[40:72] = a_str(args.volid, 32)
    pvd[80:88] = both32(total_blocks)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(BLOCK)
    pvd[132:140] = both32(pt_size)
    struct.pack_into("<I", pvd, 140, PATHTABLE_L_BLOCK)
    struct.pack_into(">I", pvd, 148, PATHTABLE_M_BLOCK)
    pvd[156:190] = dir_record(ROOTDIR_BLOCK, BLOCK, b"\x00", True)
    pvd[190:318] = a_str("", 128)
    pvd[318:446] = a_str("PHOTONOS", 128)
    pvd[446:574] = a_str("TOOLS/MKISO.PY", 128)
    pvd[574:702] = a_str("PHOTONOS MKISO", 128)
    pvd[702:739] = a_str("", 37)
    pvd[739:776] = a_str("", 37)
    pvd[776:813] = a_str("", 37)
    for off in (813, 830, 847, 864):
        pvd[off:off + 17] = b"0000000000000000\x00"
    pvd[881] = 1

    # --- Boot Record (El Torito) ----------------------------------------------------
    br = bytearray(BLOCK)
    br[0] = 0
    br[1:6] = b"CD001"
    br[6] = 1
    br[7:39] = b"EL TORITO SPECIFICATION".ljust(32, b"\x00")
    struct.pack_into("<I", br, 71, CATALOG_BLOCK)

    term = bytearray(BLOCK)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1

    # --- собираем ------------------------------------------------------------------
    iso = bytearray(total_blocks * BLOCK)

    def put(block, data):
        iso[block * BLOCK: block * BLOCK + len(data)] = data

    if not args.no_hybrid:
        put(0, make_hybrid_mbr(bytes(image[:512]), total_blocks * 4))
    put(PVD_BLOCK, pvd)
    put(BOOTREC_BLOCK, br)
    put(TERM_BLOCK, term)
    put(CATALOG_BLOCK, boot_catalog(image_block, BOOT_LOAD_SECTORS))
    put(PATHTABLE_L_BLOCK, pt_l)
    put(PATHTABLE_M_BLOCK, pt_m)
    put(ROOTDIR_BLOCK, root)
    put(image_block, bytes(image))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(iso)
    print("  iso: %s (%d bytes, %d blocks), boot image at block %d (%d blocks)%s"
          % (args.out, len(iso), total_blocks, image_block, image_blocks,
             "" if args.no_hybrid else ", hybrid MBR"))


if __name__ == "__main__":
    main()
