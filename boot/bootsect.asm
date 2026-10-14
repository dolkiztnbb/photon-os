; =============================================================================
;  PhotonOS — загрузочный сектор (stage 1)
;
;  Задача stage 1 минимальна: понять, С ЧЕГО нас загрузил BIOS, прочитать
;  первые 8 КБ образа (MBR + stage2) по адресу IMAGE_HEAD_ADDR и передать
;  управление stage2 (boot/stage2.asm). Вся «тяжёлая» логика (A20, unreal
;  mode, загрузка ядра выше 1 МБ, копирование образа в ОЗУ) живёт в stage2.
;
;  Поддерживаемые носители (всё через BIOS int 13h, без собственных драйверов):
;    * HDD / IDE / SATA(legacy) / USB-HDD .... DL>=0x80, сектор 512, LBA (AH=42h)
;    * дискета / USB-FDD ..................... DL<0x80 или нет EDD, CHS (AH=02h)
;    * CD-ROM (El Torito, no emulation) ...... сектор 2048, LBA (AH=42h)
;    * «гибридный» ISO, записанный dd на флешку: сектор 512, образ лежит
;      внутри ISO по LBA = bi_file*4
;
;  Раскладка сектора:
;    0x000  jmp start + "PHT1"
;    0x008  El Torito boot-info-table (56 байт, заполняет tools/mkiso.py)
;    0x040  pht_imghdr_t — раскладка образа (значения подставляет Makefile)
;    0x060  код
;    0x1B8  disk signature
;    0x1BE  таблица разделов (один раздел, чтобы BIOS считал флешку USB-HDD)
;    0x1FE  0xAA55
; =============================================================================

%include "boot/bootinfo.inc"

; --- раскладка образа (в 512-байтных секторах); Makefile передаёт через -D ---
%ifndef IMG_STAGE2_SECTORS
%define IMG_STAGE2_SECTORS 15
%endif
%ifndef IMG_INIT_LBA
%define IMG_INIT_LBA 60
%endif
%ifndef IMG_INIT_SECTORS
%define IMG_INIT_SECTORS 324
%endif
%ifndef IMG_FS_LBA
%define IMG_FS_LBA 384
%endif
%ifndef IMG_SECTORS
%define IMG_SECTORS 4096
%endif
%ifndef KERNEL_ADDR
%define KERNEL_ADDR 0x100000
%endif
%ifndef RAMDISK_ADDR
%define RAMDISK_ADDR 0x1000000
%endif

%if (IMG_STAGE2_SECTORS + 1) * 512 > IMAGE_HEAD_BYTES
%error "stage2 не помещается в IMAGE_HEAD_BYTES"
%endif

SCRATCH         equ 0x0900          ; буфер для int 13h AH=48h (за bootinfo)
BI              equ BOOTINFO_ADDR

[org MBR_ADDR]
[bits 16]

entry:
    jmp short start
    nop
    db "PHT1", 0                    ; 0x03..0x07

; ---- 0x08: El Torito boot-info-table (нули в сыром образе) ------------------
boot_info_table:
    times 56 db 0

; ---- 0x40: заголовок образа (pht_imghdr_t) ----------------------------------
imghdr:
    dd IMGHDR_MAGIC                 ; "PHTB"
    dw 1                            ; версия
    dw IMG_STAGE2_SECTORS
    dd IMG_INIT_LBA
    dd IMG_INIT_SECTORS
    dd IMG_FS_LBA
    dd IMG_SECTORS
    dd KERNEL_ADDR
    dd RAMDISK_ADDR

; ---- 0x60: код ----------------------------------------------------------------
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, MBR_ADDR                ; стек растёт вниз от 0x7C00
    sti
    cld
    jmp 0:.cs_ok                    ; некоторые BIOS входят как 07C0:0000
.cs_ok:

    ; --- обнуляем bootinfo и заполняем базовые поля -------------------------
    mov di, BI
    mov cx, BI_SIZE / 2
    rep stosw                       ; ax = 0
    mov dword [BI + BI_MAGIC], BOOTINFO_MAGIC
    mov [BI + BI_DRIVE], dl
    mov byte [BI + BI_SHIFT], 9     ; по умолчанию сектор 512 байт

    ; --- загрузка из ISO? (mkiso записал bi_file != 0) ----------------------
    mov eax, [boot_info_table + BIT_FILE]
    test eax, eax
    jz .not_iso
    or byte [BI + BI_FLAGS], BOOTF_ISO

    ; размер сектора носителя: int 13h AH=48h
    mov si, SCRATCH
    mov word [si], 0x1E
    mov ah, 0x48
    mov dl, [BI + BI_DRIVE]
    int 0x13
    jc .assume_cd                   ; нет ответа — считаем, что это CD
    cmp word [si + 0x18], 2048
    jne .iso_on_512                 ; гибридный ISO на флешке/диске
.assume_cd:
    mov byte [BI + BI_SHIFT], 11
    mov byte [BI + BI_MEDIA], MEDIA_CDROM
    jmp .set_image_lba
.iso_on_512:
    shl eax, 2                      ; LBA(2048) -> LBA(512)
.set_image_lba:
    mov [BI + BI_IMAGE_LBA], eax
.not_iso:

    ; --- есть ли расширения int 13h (LBA)? ----------------------------------
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BI + BI_DRIVE]
    int 0x13
    jc .chs
    cmp bx, 0xAA55
    jne .chs
    test cl, 1
    jz .chs
    or byte [BI + BI_FLAGS], BOOTF_EDD

    ; --- LBA: читаем IMAGE_HEAD_BYTES от начала образа в IMAGE_HEAD_ADDR ----
    mov cl, [BI + BI_SHIFT]
    mov ax, IMAGE_HEAD_BYTES
    shr ax, cl
    mov [dap.count], ax
    mov eax, [BI + BI_IMAGE_LBA]
    mov [dap.lba], eax
    mov si, dap
    mov ah, 0x42
    mov dl, [BI + BI_DRIVE]
    int 0x13
    jc .err_disk
    jmp .loaded

    ; --- CHS: дискета / USB-FDD / очень старый BIOS ---------------------------
.chs:
    cmp byte [BI + BI_SHIFT], 9
    jne .err_disk                   ; CD без EDD не бывает
    mov si, MBR_ADDR                ; сектор 0 у нас уже есть — копируем себя
    mov di, IMAGE_HEAD_ADDR
    mov cx, 256
    rep movsw
    mov si, 3                       ; попытки
.chs_retry:
    mov ax, 0x0200 | (IMAGE_HEAD_BYTES / 512 - 1)   ; AH=02h, AL=секторов
    mov cx, 0x0002                  ; цилиндр 0, сектор 2
    xor dh, dh                      ; головка 0
    mov dl, [BI + BI_DRIVE]
    mov bx, IMAGE_HEAD_ADDR + 512
    int 0x13
    jnc .loaded
    xor ah, ah                      ; сброс контроллера и ещё раз
    int 0x13
    dec si
    jnz .chs_retry
    jmp .err_disk

.loaded:
    cmp word [STAGE2_ADDR + 2], 0x3253      ; сигнатура "S2"
    jne .err_sig
    mov dl, [BI + BI_DRIVE]
    jmp 0:STAGE2_ADDR

; --- ошибки: печатаем "E<код>" и останавливаемся ---------------------------------
.err_disk:
    mov al, 'D'                     ; ED: ошибка чтения диска
    jmp .err
.err_sig:
    mov al, 'S'                     ; ES: stage2 не найден / повреждён
.err:
    push ax
    mov ax, 0x0E45                  ; 'E'
    xor bx, bx
    int 0x10
    pop ax
    mov ah, 0x0E
    int 0x10
.halt:
    hlt
    jmp .halt

; --- Disk Address Packet для AH=42h ------------------------------------------
dap:
    db 0x10, 0
.count: dw 0
    dw IMAGE_HEAD_ADDR, 0           ; смещение:сегмент буфера
.lba:   dq 0

; --- хвост сектора -------------------------------------------------------------
    times 0x1B8 - ($ - $$) db 0
    db "PHTN"                       ; disk signature
    dw 0
    ; раздел 1: активный, тип 0xDA (non-FS data), LBA 1 .. IMG_SECTORS-1
    db 0x80, 0x00, 0x02, 0x00, 0xDA, 0x01, 0x01, 0x04
    dd 1
    dd IMG_SECTORS - 1
    times 48 db 0                   ; разделы 2..4
    dw 0xAA55
