#!/usr/bin/env python3
"""
tools/mkimage.py — собирает сырой образ диска PhotonOS (os-image.bin).

Раскладка образа (в 512-байтных секторах) задаётся аргументами и должна
совпадать с -D флагами, с которыми собран boot/bootsect.asm (это делает
Makefile). Скрипт проверяет, что каждая часть помещается в отведённое место,
и падает с понятной ошибкой вместо молчаливого наложения файлов друг на друга.

Использование:
  mkimage.py --out os-image.bin --sectors 4096 \
      --part bootsect.bin@0:1 --part stage2.bin@1:15 \
      --part init.bin@60:324 --part fs.vd@384:
"""
import argparse
import os
import struct
import sys

SECTOR = 512


def die(msg):
    print("mkimage: ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def parse_part(spec):
    # file@lba:max_sectors   (max_sectors может быть пустым — до конца образа)
    try:
        path, where = spec.rsplit("@", 1)
        lba, _, limit = where.partition(":")
        lba = int(lba, 0)
        limit = int(limit, 0) if limit else None
    except ValueError:
        die("неверный формат --part '%s' (ожидается file@lba:max_sectors)" % spec)
    return path, lba, limit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--sectors", type=lambda v: int(v, 0), required=True,
                    help="полный размер образа в 512-байтных секторах")
    ap.add_argument("--part", action="append", default=[],
                    help="file@lba[:max_sectors] (можно несколько)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    image = bytearray(args.sectors * SECTOR)
    used = []  # (start, end, name) для проверки пересечений

    for spec in args.part:
        path, lba, limit = parse_part(spec)
        if not os.path.exists(path):
            die("файл не найден: %s" % path)
        data = open(path, "rb").read()
        nsect = (len(data) + SECTOR - 1) // SECTOR
        if limit is None:
            limit = args.sectors - lba
        if nsect > limit:
            die("%s занимает %d секторов (%d байт), а отведено %d (LBA %d..%d)"
                % (path, nsect, len(data), limit, lba, lba + limit - 1))
        if lba + nsect > args.sectors:
            die("%s выходит за пределы образа (%d секторов)" % (path, args.sectors))
        for (s, e, n) in used:
            if lba < e and lba + limit > s:
                die("регион %s (LBA %d..%d) пересекается с %s (LBA %d..%d)"
                    % (path, lba, lba + limit - 1, n, s, e - 1))
        used.append((lba, lba + limit, path))
        image[lba * SECTOR: lba * SECTOR + len(data)] = data
        if not args.quiet:
            print("  %-36s -> LBA %5d, %4d/%4d sect (%d bytes)"
                  % (os.path.basename(path), lba, nsect, limit, len(data)))

    # Небольшая проверка загрузочного сектора
    if image[510:512] != b"\x55\xAA":
        die("в секторе 0 нет сигнатуры 0xAA55 — bootsect.bin собран неверно")
    magic, = struct.unpack_from("<I", image, 0x40)
    if magic != 0x42544850:  # "PHTB"
        die("в MBR нет заголовка образа PHTB (boot/bootsect.asm устарел?)")
    hdr_image_sectors, = struct.unpack_from("<I", image, 0x40 + 0x14)
    if hdr_image_sectors != args.sectors:
        die("IMG_SECTORS в заголовке MBR (%d) != --sectors (%d): "
            "bootsect.asm собран с другими -D флагами" % (hdr_image_sectors, args.sectors))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(image)
    if not args.quiet:
        print("  image: %s (%d bytes, %d sectors)" % (args.out, len(image), args.sectors))


if __name__ == "__main__":
    main()
