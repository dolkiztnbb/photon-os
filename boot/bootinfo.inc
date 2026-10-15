; boot/bootinfo.inc — константы, общие для bootsect.asm / stage2.asm.
; Зеркало boot/bootinfo.h: при изменении правьте ОБА файла
; (согласованность проверяет tools/check_bootinfo.py при сборке).

%ifndef __PHT_BOOTINFO_INC__
%define __PHT_BOOTINFO_INC__

; --- расположение в памяти --------------------------------------------------
BOOTINFO_ADDR       equ 0x0600      ; pht_bootinfo_t
BOOTINFO_MAGIC      equ 0x544F4850  ; "PHOT"
MBR_ADDR            equ 0x7C00      ; куда BIOS кладёт первый сектор
IMAGE_HEAD_ADDR     equ 0x8000      ; копия первых 16 КБ образа (MBR + stage2)
IMAGE_HEAD_BYTES    equ 16384
STAGE2_ADDR         equ 0x8200      ; = IMAGE_HEAD_ADDR + 512, точка входа stage2
BOUNCE_SEG          equ 0x1000      ; буфер 0x10000..0x1FFFF для int 13h
BOUNCE_ADDR         equ 0x10000
BOUNCE_BYTES        equ 0x8000      ; читаем по 32 КБ за один вызов BIOS
PM_STACK_TOP        equ 0x7FFF0     ; стек ядра при входе в init(): нижняя память,
                                    ; 0x10000..0x7FFFF свободна и не пересекается с
                                    ; пулом kmalloc (старый загрузчик ставил 0x6FFFFF —
                                    ; прямо внутри пула 0x600000..0xE00000)

; --- pht_bootinfo_t -----------------------------------------------------------
BI_MAGIC            equ 0x00
BI_DRIVE            equ 0x04
BI_MEDIA            equ 0x05
BI_SHIFT            equ 0x06
BI_FLAGS            equ 0x07
BI_IMAGE_LBA        equ 0x08
BI_KERNEL_ADDR      equ 0x0C
BI_KERNEL_SECTORS   equ 0x10
BI_RAMDISK_ADDR     equ 0x14
BI_RAMDISK_SECTORS  equ 0x18
BI_IMAGE_SECTORS    equ 0x1C
BI_FS_LBA           equ 0x20
BI_MEM_TOTAL_KB     equ 0x24
BI_MBR              equ 0x40
BI_SIZE             equ 0x240

MEDIA_UNKNOWN       equ 0
MEDIA_FLOPPY        equ 1
MEDIA_HDD           equ 2
MEDIA_CDROM         equ 3

BOOTF_EDD           equ (1 << 0)
BOOTF_RAMDISK       equ (1 << 1)
BOOTF_ISO           equ (1 << 2)
BOOTF_A20           equ (1 << 3)

; --- заголовок образа в MBR (pht_imghdr_t) -----------------------------------
IMGHDR_OFFSET       equ 0x40
IMGHDR_MAGIC        equ 0x42544850  ; "PHTB"
IH_MAGIC            equ 0x00
IH_VERSION          equ 0x04
IH_STAGE2_SECTORS   equ 0x06
IH_INIT_LBA         equ 0x08
IH_INIT_SECTORS     equ 0x0C
IH_FS_LBA           equ 0x10
IH_IMAGE_SECTORS    equ 0x14
IH_KERNEL_ADDR      equ 0x18
IH_RAMDISK_ADDR     equ 0x1C

; --- El Torito boot-info-table (байты 8..63 MBR) -------------------------------
BIT_OFFSET          equ 8
BIT_PVD             equ 0
BIT_FILE            equ 4
BIT_LENGTH          equ 8
BIT_CSUM            equ 12

%endif
