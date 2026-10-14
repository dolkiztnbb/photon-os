#!/usr/bin/env python3
"""
tools/boottest.py — прогон загрузчика (boot/bootsect.asm + boot/stage2.asm) в
эмуляторе Unicorn с «фальшивым BIOS».

QEMU под рукой может не быть, а проверить, что stage1/stage2 правильно
считают LBA, грузят ядро, заполняют bootinfo и входят в защищённый режим —
хочется после каждого изменения. Скрипт эмулирует ровно те сервисы BIOS,
которыми пользуется загрузчик (int 10h/13h/15h), подсовывает ему образ как
диск с нужным размером сектора и останавливается, когда EIP == KERNEL_ADDR.

Режимы (--mode):
  hdd      — os-image.bin как HDD/USB-HDD с EDD (LBA, 512 байт), DL=80h
  chs      — то же, но BIOS без расширений int 13h: только CHS (AH=02h/08h)
  cd       — photon.iso как CD-ROM: El Torito no-emulation, сектор 2048, DL=E0h
  usb-iso  — photon.iso записан на флешку: гибридный MBR, сектор 512, DL=80h
  rdfail   — как hdd, но сектора с LBA 1000 не читаются: ядро должно
             загрузиться, а ramdisk — обрезаться (ошибка не фатальна)

Пример:
  python3 tools/boottest.py --image output/build/image/os-image.bin --mode hdd
  python3 tools/boottest.py --iso output/build/image/photon.iso --mode cd

Требует: pip install unicorn
"""
import argparse
import struct
import sys

try:
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_INSN, UC_HOOK_CODE
    from unicorn.x86_const import *
except ImportError:
    print("boottest: нужен пакет unicorn (pip install unicorn)")
    sys.exit(2)

MEM_SIZE = 64 * 1024 * 1024
BOOTINFO_ADDR = 0x600
BOOTINFO_MAGIC = 0x544F4850
KERNEL_ADDR = 0x100000
RAMDISK_ADDR = 0x1000000
PM_STACK_TOP = 0x7FFF0
IMG_INIT_LBA, IMG_INIT_SECTORS, IMG_FS_LBA = 60, 324, 384
ISO_IMAGE_BLOCK = 23

MEDIA = {0: "unknown", 1: "floppy", 2: "hdd", 3: "cdrom"}
BOOTF_EDD, BOOTF_RAMDISK, BOOTF_ISO, BOOTF_A20 = 1, 2, 4, 8


class Disk:
    def __init__(self, data, sector_size, edd, heads=16, spt=63, fail_from=None):
        self.data, self.ss, self.edd = data, sector_size, edd
        self.heads, self.spt = heads, spt
        self.total = len(data) // sector_size
        self.fail_from = fail_from     # сектора начиная с этого LBA «не читаются»

    def read(self, lba, count):
        if lba < 0 or lba + count > self.total:
            return None
        if self.fail_from is not None and lba + count > self.fail_from:
            return None
        return self.data[lba * self.ss:(lba + count) * self.ss]


class Harness:
    def __init__(self, disk, drive, boot_bytes):
        self.disk, self.drive = disk, drive
        self.tty = bytearray()
        self.stats = {}
        self.errors = []
        self.reached_kernel = False
        uc = self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        uc.mem_map(0, MEM_SIZE)
        uc.mem_write(0x7C00, boot_bytes)
        uc.hook_add(UC_HOOK_INTR, self.on_intr)
        uc.hook_add(UC_HOOK_INSN, self.on_in, None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN, self.on_out, None, 1, 0, UC_X86_INS_OUT)
        uc.hook_add(UC_HOOK_CODE, self.on_kernel, None, KERNEL_ADDR, KERNEL_ADDR)
        for r in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
            uc.reg_write(r, 0)
        uc.reg_write(UC_X86_REG_ESP, 0x7000)   # BIOS оставляет что попало; stage1 ставит свой
        uc.reg_write(UC_X86_REG_EDX, drive)

    # --- регистры -------------------------------------------------------------
    def r(self, reg):
        return self.uc.reg_read(reg)

    def w(self, reg, val):
        self.uc.reg_write(reg, val)

    def set_cf(self, cf):
        fl = self.r(UC_X86_REG_EFLAGS)
        self.w(UC_X86_REG_EFLAGS, (fl | 1) if cf else (fl & ~1))

    def set_ah(self, v):
        self.w(UC_X86_REG_AH, v & 0xFF)

    def lin(self, seg_reg, off):
        return (self.r(seg_reg) << 4) + off

    def count(self, key):
        self.stats[key] = self.stats.get(key, 0) + 1

    # --- хуки --------------------------------------------------------------------
    def on_kernel(self, uc, addr, size, user):
        self.reached_kernel = True
        uc.emu_stop()

    def on_in(self, uc, port, size, user):
        if port == 0x92:
            return 0x02
        if port == 0x64:
            return 0x00
        return 0

    def on_out(self, uc, port, size, value, user):
        if port == 0x92 and (value & 1):
            self.errors.append("запись бита reset в порт 0x92!")

    def on_intr(self, uc, intno, user):
        ah = self.r(UC_X86_REG_AH)
        if intno == 0x10:
            if ah == 0x0E:
                self.tty.append(self.r(UC_X86_REG_AL))
            return
        if intno == 0x13:
            return self.int13(ah)
        if intno == 0x15:
            return self.int15()
        self.errors.append("неожиданное прерывание int %02Xh (AH=%02X)" % (intno, ah))
        self.set_cf(1)

    def int15(self):
        ax = self.r(UC_X86_REG_AX)
        if ax == 0x2401:
            self.count("int15 2401 (A20)")
            self.set_ah(0); self.set_cf(0)
        elif ax == 0x2402:
            self.set_ah(0); self.w(UC_X86_REG_AL, 1); self.set_cf(0)
        elif ax == 0xE801:
            self.count("int15 E801")
            kb_low = 15 * 1024                       # 1..16 МБ
            blk_high = (MEM_SIZE - 16 * 1024 * 1024) // 65536
            self.w(UC_X86_REG_AX, kb_low); self.w(UC_X86_REG_BX, blk_high)
            self.w(UC_X86_REG_CX, kb_low); self.w(UC_X86_REG_DX, blk_high)
            self.set_cf(0)
        else:
            self.set_ah(0x86); self.set_cf(1)

    def int13(self, ah):
        dl = self.r(UC_X86_REG_DL)
        if dl != self.drive:
            self.errors.append("int 13h AH=%02X для чужого диска DL=%02X" % (ah, dl))
            self.set_ah(0x01); self.set_cf(1)
            return
        d = self.disk
        if ah == 0x00:
            self.count("int13 00 (reset)")
            self.set_ah(0); self.set_cf(0)
        elif ah == 0x02:
            self.count("int13 02 (CHS read)")
            if d.ss != 512:
                self.set_ah(0x01); self.set_cf(1); return
            al = self.r(UC_X86_REG_AL)
            ch, cl, dh = self.r(UC_X86_REG_CH), self.r(UC_X86_REG_CL), self.r(UC_X86_REG_DH)
            cyl = ch | ((cl & 0xC0) << 2)
            sec = cl & 0x3F
            if sec == 0 or sec > d.spt or dh >= d.heads or al == 0 or sec - 1 + al > d.spt:
                self.errors.append("int13 02: некорректные C/H/S/count %d/%d/%d/%d" % (cyl, dh, sec, al))
                self.set_ah(0x04); self.set_cf(1); return
            lba = (cyl * d.heads + dh) * d.spt + (sec - 1)
            data = d.read(lba, al)
            if data is None:
                self.set_ah(0x04); self.set_cf(1); return
            dst = self.lin(UC_X86_REG_ES, self.r(UC_X86_REG_BX))
            if (dst & 0xFFFF) + len(data) > 0x10000:
                self.errors.append("int13 02: буфер пересекает границу 64 КБ (%x)" % dst)
            self.uc.mem_write(dst, data)
            self.set_ah(0); self.set_cf(0)
        elif ah == 0x08:
            self.count("int13 08 (geometry)")
            if d.ss != 512:
                self.set_ah(0x01); self.set_cf(1); return
            cyls = max(1, d.total // (d.heads * d.spt))
            maxc = min(cyls - 1, 1023)
            self.w(UC_X86_REG_CH, maxc & 0xFF)
            self.w(UC_X86_REG_CL, d.spt | ((maxc >> 8) << 6))
            self.w(UC_X86_REG_DH, d.heads - 1)
            self.w(UC_X86_REG_DL, 1)
            self.w(UC_X86_REG_ES, 0); self.w(UC_X86_REG_DI, 0)
            self.set_ah(0); self.set_cf(0)
        elif ah == 0x41:
            self.count("int13 41 (EDD check)")
            if not d.edd:
                self.set_ah(0x01); self.set_cf(1); return
            self.w(UC_X86_REG_BX, 0xAA55); self.w(UC_X86_REG_CX, 0x0007)
            self.set_ah(0x30); self.set_cf(0)
        elif ah == 0x42:
            self.count("int13 42 (LBA read)")
            if not d.edd:
                self.set_ah(0x01); self.set_cf(1); return
            dap = self.uc.mem_read(self.lin(UC_X86_REG_DS, self.r(UC_X86_REG_SI)), 16)
            size, _, cnt, off, seg, lba = struct.unpack("<BBHHHQ", bytes(dap))
            if size < 16 or cnt > 127:
                self.errors.append("int13 42: плохой DAP size=%d count=%d" % (size, cnt))
                self.set_ah(0x01); self.set_cf(1); return
            data = d.read(lba, cnt)
            if data is None:
                if lba + cnt > d.total:
                    self.errors.append("int13 42: чтение за пределами диска lba=%d count=%d" % (lba, cnt))
                self.count("int13 42 (failed)")
                self.set_ah(0x04); self.set_cf(1); return
            dst = (seg << 4) + off
            if off + len(data) > 0x10000:
                self.errors.append("int13 42: буфер %04X:%04X+%d выходит за сегмент" % (seg, off, len(data)))
            self.uc.mem_write(dst, data)
            self.set_ah(0); self.set_cf(0)
        elif ah == 0x48:
            self.count("int13 48 (drive params)")
            if not d.edd:
                self.set_ah(0x01); self.set_cf(1); return
            buf = self.lin(UC_X86_REG_DS, self.r(UC_X86_REG_SI))
            bufsize, = struct.unpack("<H", bytes(self.uc.mem_read(buf, 2)))
            if bufsize < 0x1A:
                self.errors.append("int13 48: размер буфера %d < 0x1A" % bufsize)
                self.set_ah(0x01); self.set_cf(1); return
            params = struct.pack("<HHIIIQH", 0x1A, 0x0002 if d.ss == 512 else 0x0004,
                                 0, 0, 0, d.total, d.ss)
            self.uc.mem_write(buf, params)
            self.set_ah(0); self.set_cf(0)
        elif ah == 0x4B:
            self.count("int13 4B (El Torito status)")
            self.set_ah(0x01); self.set_cf(1)
        else:
            self.errors.append("int 13h: неподдерживаемая функция AH=%02X" % ah)
            self.set_ah(0x01); self.set_cf(1)

    # --- запуск -------------------------------------------------------------------
    def run(self, timeout_s=60):
        try:
            self.uc.emu_start(0x7C00, KERNEL_ADDR, timeout=timeout_s * 1000 * 1000)
        except Exception as e:  # noqa
            self.errors.append("эмуляция прервана: %s (EIP=%x)" % (e, self.r(UC_X86_REG_EIP)))
        if self.r(UC_X86_REG_EIP) == KERNEL_ADDR and (self.r(UC_X86_REG_CR0) & 1):
            self.reached_kernel = True


# --- проверки после остановки ---------------------------------------------------------

def decode_descriptor(raw):
    lo, hi = struct.unpack("<II", raw)
    base = (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000)
    limit = (lo & 0xFFFF) | (hi & 0x000F0000)
    access = (hi >> 8) & 0xFF
    flags = (hi >> 20) & 0xF
    return base, limit, access, flags


def check(h, image, expect):
    uc = h.uc
    ok = True

    def fail(msg):
        nonlocal ok
        ok = False
        print("  FAIL: " + msg)

    def expect_eq(name, got, want):
        if got != want:
            fail("%s = %s, ожидалось %s" % (name, hex(got) if isinstance(got, int) else got,
                                           hex(want) if isinstance(want, int) else want))

    if not h.reached_kernel:
        fail("ядро не достигнуто, EIP=%x" % h.r(UC_X86_REG_EIP))
        return False

    # --- состояние процессора на входе в ядро ---
    expect_eq("CR0.PE", h.r(UC_X86_REG_CR0) & 1, 1)
    expect_eq("CR0.PG", h.r(UC_X86_REG_CR0) >> 31, 0)
    expect_eq("CS", h.r(UC_X86_REG_CS), 0x08)
    for name, reg in (("DS", UC_X86_REG_DS), ("ES", UC_X86_REG_ES), ("FS", UC_X86_REG_FS),
                      ("GS", UC_X86_REG_GS), ("SS", UC_X86_REG_SS)):
        expect_eq(name, h.r(reg), 0x10)
    expect_eq("ESP", h.r(UC_X86_REG_ESP), PM_STACK_TOP)
    expect_eq("EAX", h.r(UC_X86_REG_EAX), BOOTINFO_MAGIC)
    expect_eq("EBX", h.r(UC_X86_REG_EBX), BOOTINFO_ADDR)
    expect_eq("IF", (h.r(UC_X86_REG_EFLAGS) >> 9) & 1, 0)

    # --- GDT ---
    gdt_base, gdt_limit = h.r(UC_X86_REG_GDTR)[1], h.r(UC_X86_REG_GDTR)[2]
    if gdt_limit < 23:
        fail("GDT слишком маленькая: limit=%d" % gdt_limit)
    else:
        for idx, want_access in ((1, 0x9A), (2, 0x92)):
            raw = bytes(uc.mem_read(gdt_base + idx * 8, 8))
            base, limit, access, flags = decode_descriptor(raw)
            access &= 0xFE   # бит Accessed выставляет сам процессор
            if base != 0 or limit != 0xFFFFF or access != want_access or flags != 0xC:
                fail("дескриптор GDT[%d] = %s (base=%x limit=%x access=%x flags=%x)"
                     % (idx, raw.hex(), base, limit, access, flags))

    # --- ядро в памяти ---
    kernel_mem = bytes(uc.mem_read(KERNEL_ADDR, IMG_INIT_SECTORS * 512))
    kernel_img = image[IMG_INIT_LBA * 512:(IMG_INIT_LBA + IMG_INIT_SECTORS) * 512]
    if kernel_mem != kernel_img:
        first = next(i for i in range(len(kernel_mem)) if kernel_mem[i] != kernel_img[i])
        fail("ядро по 0x%X не совпадает с образом (первое расхождение +0x%X)" % (KERNEL_ADDR, first))

    # --- bootinfo ---
    bi = bytes(uc.mem_read(BOOTINFO_ADDR, 0x240))
    (magic, drive, media, shift, flags, image_lba, kaddr, ksect, rd_addr, rd_sect,
     img_sect, fs_lba, mem_kb) = struct.unpack_from("<IBBBBIIIIIIII", bi, 0)
    mbr = bi[0x40:0x240]
    expect_eq("bootinfo.magic", magic, BOOTINFO_MAGIC)
    expect_eq("bootinfo.bios_drive", drive, expect["drive"])
    expect_eq("bootinfo.media_type", MEDIA.get(media), expect["media"])
    expect_eq("bootinfo.sector_shift", shift, expect["shift"])
    expect_eq("bootinfo.flags", flags, expect["flags"])
    expect_eq("bootinfo.image_lba", image_lba, expect["image_lba"])
    expect_eq("bootinfo.kernel_addr", kaddr, KERNEL_ADDR)
    expect_eq("bootinfo.kernel_sectors", ksect, IMG_INIT_SECTORS)
    expect_eq("bootinfo.ramdisk_addr", rd_addr, RAMDISK_ADDR)
    want_rd = expect.get("ramdisk_sectors", len(image) // 512)
    expect_eq("bootinfo.ramdisk_sectors", rd_sect, want_rd)
    expect_eq("bootinfo.image_sectors", img_sect, len(image) // 512)
    expect_eq("bootinfo.fs_lba", fs_lba, IMG_FS_LBA)
    expect_eq("bootinfo.mem_total_kb", mem_kb, MEM_SIZE // 1024)
    # stage2 кладёт сюда первый сектор ОБРАЗА (с image_lba), а не сектор 0 носителя:
    # у гибридного ISO на флешке это разные сектора (разные таблицы разделов)
    if mbr != image[:512]:
        fail("bootinfo.mbr не совпадает с первым сектором образа")

    # --- ramdisk ---
    rd = bytes(uc.mem_read(RAMDISK_ADDR, want_rd * 512))
    if rd != image[:want_rd * 512]:
        first = next(i for i in range(len(rd)) if rd[i] != image[i])
        fail("ramdisk по 0x%X не совпадает с образом (первое расхождение +0x%X)" % (RAMDISK_ADDR, first))

    # --- загрузчик не должен трогать IVT/BDA ---
    low = bytes(uc.mem_read(0, 0x500))
    if any(low):
        fail("загрузчик записал что-то в IVT/BDA (0..0x4FF)")

    for e in h.errors:
        fail(e)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default="output/build/image/os-image.bin")
    ap.add_argument("--iso", default="output/build/image/photon.iso")
    ap.add_argument("--mode", choices=("hdd", "chs", "cd", "usb-iso", "rdfail", "all"), default="all")
    ap.add_argument("--heads", type=int, default=16)
    ap.add_argument("--spt", type=int, default=63)
    ap.add_argument("--verbose", "-v", action="store_true", help="показать вывод загрузчика")
    args = ap.parse_args()

    modes = ("hdd", "chs", "cd", "usb-iso", "rdfail") if args.mode == "all" else (args.mode,)
    image = open(args.image, "rb").read()
    all_ok = True

    for mode in modes:
        if mode in ("cd", "usb-iso"):
            iso = open(args.iso, "rb").read()
            img_in_iso = iso[ISO_IMAGE_BLOCK * 2048: ISO_IMAGE_BLOCK * 2048 + len(image)]
            if img_in_iso[64:] != image[64:]:
                print("%s: образ внутри ISO не соответствует %s" % (mode, args.image))
                all_ok = False
                continue
        if mode == "hdd":
            disk, drive = Disk(image, 512, True, args.heads, args.spt), 0x80
            boot = image[:512]
            expect = dict(drive=0x80, media="hdd", shift=9, image_lba=0,
                          flags=BOOTF_EDD | BOOTF_A20 | BOOTF_RAMDISK)
            ref_image = image
        elif mode == "rdfail":
            disk, drive = Disk(image, 512, True, args.heads, args.spt, fail_from=1000), 0x80
            boot = image[:512]
            expect = dict(drive=0x80, media="hdd", shift=9, image_lba=0,
                          flags=BOOTF_EDD | BOOTF_A20 | BOOTF_RAMDISK,
                          ramdisk_sectors=1000 // 64 * 64)
            ref_image = image
        elif mode == "chs":
            disk, drive = Disk(image, 512, False, args.heads, args.spt), 0x80
            boot = image[:512]
            expect = dict(drive=0x80, media="floppy", shift=9, image_lba=0,
                          flags=BOOTF_A20 | BOOTF_RAMDISK)
            ref_image = image
        elif mode == "cd":
            disk, drive = Disk(iso, 2048, True), 0xE0
            boot = img_in_iso[:512]
            expect = dict(drive=0xE0, media="cdrom", shift=11, image_lba=ISO_IMAGE_BLOCK,
                          flags=BOOTF_EDD | BOOTF_ISO | BOOTF_A20 | BOOTF_RAMDISK)
            ref_image = img_in_iso
        else:  # usb-iso
            disk, drive = Disk(iso, 512, True, args.heads, args.spt), 0x80
            boot = iso[:512]
            expect = dict(drive=0x80, media="hdd", shift=9, image_lba=ISO_IMAGE_BLOCK * 4,
                          flags=BOOTF_EDD | BOOTF_ISO | BOOTF_A20 | BOOTF_RAMDISK)
            ref_image = img_in_iso

        print("== %s ==" % mode)
        h = Harness(disk, drive, boot)
        h.run()
        ok = check(h, ref_image, expect)
        tty = h.tty.decode("cp437", "replace")
        if args.verbose or not ok:
            print("  --- вывод загрузчика ---")
            for line in tty.splitlines():
                print("  | " + line)
            print("  --- вызовы BIOS: %s" % ", ".join("%s x%d" % kv for kv in sorted(h.stats.items())))
        print("  %s" % ("OK" if ok else "FAILED"))
        all_ok = all_ok and ok

    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
