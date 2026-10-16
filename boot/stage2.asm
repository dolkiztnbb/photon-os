; =============================================================================
;  PhotonOS — stage 2 загрузчика
;
;  Запускается из bootsect.asm по адресу STAGE2_ADDR (0x8200) в реальном
;  режиме: DS=ES=SS=0, SP=0x7C00, DL = загрузочный диск, bootinfo (0x600)
;  частично заполнен (magic, drive, sector_shift, image_lba, flags).
;  Копия первых 16 КБ образа (MBR + stage2) лежит по IMAGE_HEAD_ADDR.
; =============================================================================

%include "boot/bootinfo.inc"

BI              equ BOOTINFO_ADDR
HDR             equ IMAGE_HEAD_ADDR + IMGHDR_OFFSET
KBC_TIMEOUT     equ 0xFFFF

; --- дополнительные адреса для FS-загрузчика ---------------------------------
FS_NODES_ADDR       equ 0xC000      ; куда грузим таблицу узлов DATSLAF (до ~7KB)
FS_LIST_ADDR        equ 0xE000      ; список id ядер в /sys/kernel (до 100*4)
FS_TMP_NAME_ADDR    equ 0xE400      ; временный буфер для имени из boot.cfg
FS_BOOTCFG_BUF      equ 0xE600      ; буфер для содержимого boot.cfg (512 байт)
FS_MAGIC            equ 0xDA751AF5
NODE_SIZE           equ 64
NODE_TYPE_OFF       equ 0
NODE_NAME_OFF       equ 1
NODE_SIZE_FIELD_OFF equ 33
NODE_PARENT_OFF     equ 45
NODE_DATA_BLOCK_OFF equ 49
TYPE_FILE           equ 0
TYPE_DIR            equ 1

[org STAGE2_ADDR]
[bits 16]

    jmp short start
    dw 0x3253                       ; "S2" — сигнатура, проверяется stage1
    db "PhotonOS stage2", 0

; =============================================================================
start:
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax

    call vga_disable_blink

    mov si, msg_banner
    call puts

    ; --- проверяем заголовок образа ------------------------------------------
    cmp dword [HDR + IH_MAGIC], IMGHDR_MAGIC
    jne .bad_header

    ; --- тип носителя ----------------------------------------------------------
    mov si, msg_drive
    call puts
    movzx eax, byte [BI + BI_DRIVE]
    call print_hex8

    cmp byte [BI + BI_MEDIA], MEDIA_CDROM
    je .media_cd
    test byte [BI + BI_FLAGS], BOOTF_EDD
    jz .media_floppy
    mov byte [BI + BI_MEDIA], MEDIA_HDD
    mov si, msg_hdd
    jmp .media_done
.media_floppy:
    mov byte [BI + BI_MEDIA], MEDIA_FLOPPY
    call get_geometry
    mov si, msg_floppy
    jmp .media_done
.media_cd:
    mov si, msg_cdrom
.media_done:
    call puts
    test byte [BI + BI_FLAGS], BOOTF_ISO
    jz .no_iso
    mov si, msg_iso
    call puts
.no_iso:
    mov si, msg_image_lba
    call puts
    mov eax, [BI + BI_IMAGE_LBA]
    call print_hex32
    call newline

    ; --- переносим параметры из заголовка образа в bootinfo -------------------
    mov eax, [HDR + IH_KERNEL_ADDR]
    mov [BI + BI_KERNEL_ADDR], eax
    mov eax, [HDR + IH_IMAGE_SECTORS]
    mov [BI + BI_IMAGE_SECTORS], eax
    mov eax, [HDR + IH_FS_LBA]
    mov [BI + BI_FS_LBA], eax

    mov si, IMAGE_HEAD_ADDR
    mov di, BI + BI_MBR
    mov cx, 256
    rep movsw

    ; --- объём памяти ----------------------------------------------------------
    call detect_memory

    ; --- A20 -------------------------------------------------------------------
    call enable_a20
    jc .a20_failed
    or byte [BI + BI_FLAGS], BOOTF_A20

    ; --- попытка загрузить ядро из ФС /sys/kernel -----------------------------
    call fs_try_load_kernel
    jnc .after_kernel

    ; --- fallback: ядро по фиксированному LBA (старый метод) ------------------
    mov si, msg_kernel_lba
    call puts
    mov eax, [HDR + IH_INIT_LBA]
    mov ecx, [HDR + IH_INIT_SECTORS]
    mov edi, [HDR + IH_KERNEL_ADDR]
    call load_image_region
    jc .kernel_failed
    mov [BI + BI_KERNEL_SECTORS], ecx
    mov si, msg_ok
    call puts
    jmp .after_kernel

.after_kernel:

    ; --- копия образа в ОЗУ (ramdisk) ------------------------------------------
    mov si, msg_ramdisk
    call puts
    mov eax, [HDR + IH_RAMDISK_ADDR]
    mov ecx, [HDR + IH_IMAGE_SECTORS]
    shl ecx, 9
    add eax, ecx
    mov edx, [BI + BI_MEM_TOTAL_KB]
    test edx, edx
    jz .ramdisk_mem_ok
    shl edx, 10
    cmp eax, edx
    ja .ramdisk_skip
.ramdisk_mem_ok:
    xor eax, eax
    mov ecx, [HDR + IH_IMAGE_SECTORS]
    mov edi, [HDR + IH_RAMDISK_ADDR]
    call load_image_region
    test ecx, ecx
    jz .ramdisk_skip
    mov [BI + BI_RAMDISK_SECTORS], ecx
    mov eax, [HDR + IH_RAMDISK_ADDR]
    mov [BI + BI_RAMDISK_ADDR], eax
    or byte [BI + BI_FLAGS], BOOTF_RAMDISK
    mov si, msg_ok
    cmp ecx, [HDR + IH_IMAGE_SECTORS]
    jae .ramdisk_msg
    mov si, msg_partial
.ramdisk_msg:
    call puts
    jmp .ramdisk_done
.ramdisk_skip:
    mov si, msg_skipped
    call puts
.ramdisk_done:

    ; --- запуск ------------------------------------------------------------------
    mov si, msg_jump
    call puts
    jmp enter_protected_mode

.bad_header:
    mov si, msg_err_header
    jmp fatal
.a20_failed:
    mov si, msg_err_a20
    jmp fatal
.kernel_failed:
    mov si, msg_err_kernel
    jmp fatal

; =============================================================================
fatal:
    push si
    mov si, msg_err
    call puts
    pop si
    call puts
.halt:
    hlt
    jmp .halt

; =============================================================================
;  FS-загрузчик: загрузка ядра из /sys/kernel
; =============================================================================
fs_try_load_kernel:
    pushad
    mov si, msg_fs_try
    call puts

    ; --- читаем суперблок ФС -------------------------------------------------
    mov eax, [BI + BI_FS_LBA]
    mov [fs_lba_512], eax
    test eax, eax
    jz .fail

    mov eax, [fs_lba_512]
    mov ecx, 1
    mov edi, BOUNCE_ADDR
    call load_image_region
    jc .fail

    mov eax, [dword BOUNCE_ADDR]
    cmp eax, dword FS_MAGIC
    jne .fail

    mov eax, [dword BOUNCE_ADDR + 4]
    mov [fs_total_nodes], eax
    test eax, eax
    jz .fail
    cmp eax, 100
    ja .fail

    ; sectors for nodes = (total_nodes*64+511)/512
    mov ebx, eax
    shl ebx, 6
    add ebx, 511
    shr ebx, 9
    mov [fs_nodes_sectors], ebx

    ; --- грузим таблицу узлов в FS_NODES_ADDR --------------------------------
    mov eax, [fs_lba_512]
    inc eax
    mov ecx, ebx
    mov edi, FS_NODES_ADDR
    call load_image_region
    jc .fail

    ; --- ищем корень -----------------------------------------------------------
    call fs_find_root
    jc .fail
    mov [fs_root_id], eax

    ; --- ищем /sys -------------------------------------------------------------
    mov eax, [fs_root_id]
    mov si, name_sys
    call fs_find_child
    jc .fail
    mov [fs_sys_id], eax

    ; --- ищем /sys/kernel ------------------------------------------------------
    mov eax, [fs_sys_id]
    mov si, name_kernel
    call fs_find_child
    jc .fail
    mov [fs_kernel_dir_id], eax

    ; --- пробуем прочитать boot.cfg / default для определения ядра по умолчанию -
    mov byte [FS_TMP_NAME_ADDR], 0

    mov eax, [fs_kernel_dir_id]
    mov si, name_bootcfg
    call fs_find_child
    jnc .read_bootcfg
    mov eax, [fs_kernel_dir_id]
    mov si, name_default
    call fs_find_child
    jnc .read_bootcfg
    mov eax, [fs_kernel_dir_id]
    mov si, name_active
    call fs_find_child
    jnc .read_bootcfg
    jmp .select_kernel

.read_bootcfg:
    mov [fs_bootcfg_id], eax
    mov edi, FS_BOOTCFG_BUF
    call fs_load_file_by_id
    jc .select_kernel

    mov si, FS_BOOTCFG_BUF
    mov di, FS_TMP_NAME_ADDR
    call fs_parse_filename

.select_kernel:
    ; Всегда отображаем меню со списком ядер и ждём выбор пользователя
    call fs_list_kernels_and_select
    jc .fail
    mov [fs_kernel_file_id], eax

.load_kernel_file:
    mov ebx, FS_NODES_ADDR
    mov ecx, [fs_kernel_file_id]
    shl ecx, 6
    add ebx, ecx
    mov eax, [ebx + NODE_SIZE_FIELD_OFF]
    mov edx, [ebx + NODE_DATA_BLOCK_OFF]
    mov [fs_kernel_size], eax
    mov [fs_kernel_data_block], edx
    test eax, eax
    jz .fail

    mov si, msg_loading_fs_kernel
    call puts
    lea si, [bx + NODE_NAME_OFF]
    call puts
    mov si, msg_newline
    call puts

    mov eax, [fs_lba_512]
    add eax, edx
    mov ecx, [fs_kernel_size]
    add ecx, 511
    shr ecx, 9
    mov edi, [HDR + IH_KERNEL_ADDR]
    call load_image_region
    jc .fail

    mov eax, [fs_kernel_size]
    add eax, 511
    shr eax, 9
    mov [BI + BI_KERNEL_SECTORS], eax

    mov si, msg_fs_kernel_ok
    call puts
    popad
    clc
    ret

.fail:
    mov si, msg_fs_kernel_fail
    call puts
    popad
    stc
    ret

; -----------------------------------------------------------------------------
fs_find_root:
    push ebx
    push ecx
    push edx
    mov ecx, [fs_total_nodes]
    xor edx, edx
.loop:
    cmp edx, ecx
    jae .notfound
    mov ebx, FS_NODES_ADDR
    mov eax, edx
    shl eax, 6
    add ebx, eax
    mov eax, [ebx + NODE_PARENT_OFF]
    cmp eax, -1
    je .found
    inc edx
    jmp .loop
.found:
    mov eax, edx
    pop edx
    pop ecx
    pop ebx
    clc
    ret
.notfound:
    pop edx
    pop ecx
    pop ebx
    stc
    ret

; -----------------------------------------------------------------------------
fs_find_child:
    push ecx
    push edx
    push edi
    push esi
    mov [fs_find_parent], eax
    mov [fs_find_name_ptr], si
    mov ecx, [fs_total_nodes]
    xor edx, edx
.loop:
    cmp edx, ecx
    jae .notfound
    mov ebx, FS_NODES_ADDR
    mov eax, edx
    shl eax, 6
    add ebx, eax
    mov eax, [ebx + NODE_PARENT_OFF]
    cmp eax, [fs_find_parent]
    jne .next

    mov si, [fs_find_name_ptr]
    lea di, [bx + NODE_NAME_OFF]
    call fs_streq
    jnc .found
.next:
    inc edx
    jmp .loop
.found:
    mov eax, edx
    pop esi
    pop edi
    pop edx
    pop ecx
    clc
    ret
.notfound:
    pop esi
    pop edi
    pop edx
    pop ecx
    stc
    ret

fs_find_parent:   dd 0
fs_find_name_ptr: dw 0

; -----------------------------------------------------------------------------
fs_streq:
    push ax
    push si
    push di
    push cx
    mov cx, 32
.loop:
    lodsb
    mov ah, [di]
    cmp al, ah
    jne .noteq
    test al, al
    jz .eq
    inc di
    loop .loop
    lodsb
    test al, al
    jz .eq
    jmp .noteq
.eq:
    pop cx
    pop di
    pop si
    pop ax
    clc
    ret
.noteq:
    pop cx
    pop di
    pop si
    pop ax
    stc
    ret

; -----------------------------------------------------------------------------
fs_load_file_by_id:
    push eax
    push ebx
    push ecx
    push edx
    push edi
    mov ebx, FS_NODES_ADDR
    shl eax, 6
    add ebx, eax
    mov eax, [ebx + NODE_SIZE_FIELD_OFF]
    mov edx, [ebx + NODE_DATA_BLOCK_OFF]
    test eax, eax
    jz .fail
    mov ecx, eax
    add ecx, 511
    shr ecx, 9
    mov eax, [fs_lba_512]
    add eax, edx
    call load_image_region
    jmp .out
.fail:
    stc
.out:
    pop edi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; -----------------------------------------------------------------------------
fs_parse_filename:
    push ax
    push si
    push di
    push cx
    mov cx, 64
.skip_spaces:
    lodsb
    test al, al
    jz .end
    cmp al, ' '
    je .skip_spaces
    cmp al, 9
    je .skip_spaces
    cmp al, 13
    je .skip_spaces
    cmp al, 10
    je .skip_spaces
    jmp .copy_char
.copy_loop:
    lodsb
    test al, al
    jz .end
    cmp al, 13
    je .end
    cmp al, 10
    je .end
    cmp al, ' '
    je .end
    cmp al, 9
    je .end
.copy_char:
    stosb
    loop .copy_loop
.end:
    mov al, 0
    stosb
    pop cx
    pop di
    pop si
    pop ax
    ret

; -----------------------------------------------------------------------------
; fs_list_kernels_and_select: сбор файлов ядер, вывод списка и интерактивный выбор
; -----------------------------------------------------------------------------
fs_list_kernels_and_select:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov eax, [fs_kernel_dir_id]
    mov [fs_find_parent], eax
    mov ecx, [fs_total_nodes]
    xor edx, edx
    xor ebx, ebx
    mov edi, FS_LIST_ADDR

.collect_loop:
    cmp edx, ecx
    jae .collect_done
    push ebx
    push edi
    mov ebx, FS_NODES_ADDR
    mov eax, edx
    shl eax, 6
    add ebx, eax
    mov eax, [ebx + NODE_PARENT_OFF]
    cmp eax, [fs_find_parent]
    jne .collect_next
    mov al, [ebx + NODE_TYPE_OFF]
    cmp al, TYPE_FILE
    jne .collect_next

    ; Фильтруем служебные файлы (boot.cfg, default, active, versions.txt, README.txt)
    lea di, [bx + NODE_NAME_OFF]

    mov si, name_bootcfg
    call fs_streq
    jnc .collect_next

    mov si, name_default
    call fs_streq
    jnc .collect_next

    mov si, name_active
    call fs_streq
    jnc .collect_next

    mov si, name_versions
    call fs_streq
    jnc .collect_next

    mov si, name_readme
    call fs_streq
    jnc .collect_next

    ; Сохраняем id узла-кандидата в FS_LIST_ADDR
    pop edi
    pop ebx
    mov eax, edx
    stosd
    inc ebx
    jmp .collect_cont

.collect_next:
    pop edi
    pop ebx
.collect_cont:
    inc edx
    jmp .collect_loop

.collect_done:
    mov [fs_kernel_count], ebx
    test ebx, ebx
    jz .fail

    ; --- Находим индекс ядра по умолчанию ---
    mov dword [fs_default_kernel_idx], 0
    xor edx, edx
.find_def_loop:
    cmp edx, [fs_kernel_count]
    jae .find_def_done

    mov bx, FS_LIST_ADDR
    mov ax, dx
    shl ax, 2
    add bx, ax
    mov eax, [bx]          ; node id
    mov ebx, FS_NODES_ADDR
    shl eax, 6
    add ebx, eax
    lea di, [bx + NODE_NAME_OFF]

    ; Если задано имя из boot.cfg — сверяем с ним
    mov si, FS_TMP_NAME_ADDR
    cmp byte [si], 0
    je .check_def_bin
    call fs_streq
    jnc .found_def_match

.check_def_bin:
    mov si, name_kernel_bin
    call fs_streq
    jnc .found_def_match

    inc edx
    jmp .find_def_loop

.found_def_match:
    mov [fs_default_kernel_idx], edx

.find_def_done:
    ; --- Выводим список доступных ядер ---
    mov si, msg_kernel_list_header
    call puts

    xor edx, edx
.print_loop:
    cmp edx, [fs_kernel_count]
    jae .ask

    mov si, msg_kernel_list_item
    call puts
    mov al, '0'
    add al, dl
    call putc
    mov al, ')'
    call putc
    mov al, ' '
    call putc

    ; Имя файла
    mov bx, FS_LIST_ADDR
    mov ax, dx
    shl ax, 2
    add bx, ax
    mov eax, [bx]
    mov ebx, FS_NODES_ADDR
    shl eax, 6
    add ebx, eax
    lea si, [bx + NODE_NAME_OFF]
    call puts

    ; Пометка [default]
    cmp edx, [fs_default_kernel_idx]
    jne .print_no_default
    mov si, msg_default_tag
    call puts

.print_no_default:
    call newline
    inc edx
    jmp .print_loop

.ask:
    mov si, msg_kernel_select_prompt
    call puts
    mov al, '0'
    add al, byte [fs_default_kernel_idx]
    call putc
    mov si, msg_prompt_end
    call puts

.wait_key:
    mov ah, 0
    int 0x16                ; Ожидание клавиши (BIOS int 16h)
    cmp al, 13              ; Enter
    je .select_default
    cmp al, '0'
    jb .wait_key
    cmp al, '9'
    ja .wait_key
    sub al, '0'
    movzx edx, al
    cmp edx, [fs_kernel_count]
    jae .wait_key

    ; Выводим выбранную цифру на экран
    call putc
    call newline
    jmp .selected

.select_default:
    mov al, '0'
    add al, byte [fs_default_kernel_idx]
    call putc
    call newline
    mov edx, [fs_default_kernel_idx]

.selected:
    mov bx, FS_LIST_ADDR
    mov ax, dx
    shl ax, 2
    add bx, ax
    mov eax, [bx]          ; Возвращаем выбранный node id в EAX
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    clc
    ret

.fail:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    stc
    ret

; =============================================================================
;  load_image_region
; =============================================================================
load_image_region:
    pushad
    mov [.dest], edi
    mov [.remaining], ecx
    mov dword [.loaded], 0

    mov cl, [BI + BI_SHIFT]
    sub cl, 9
    mov [.subshift], cl
    shr eax, cl
    add eax, [BI + BI_IMAGE_LBA]
    mov [.cur_lba], eax
    mov eax, [.remaining]
    mov edx, 1
    shl edx, cl
    dec edx
    add eax, edx
    shr eax, cl
    mov [.remaining], eax

.loop:
    mov eax, [.remaining]
    test eax, eax
    jz .done

    mov ebx, BOUNCE_BYTES
    mov cl, [BI + BI_SHIFT]
    shr ebx, cl
    cmp eax, ebx
    jbe .chunk_ok
    mov eax, ebx
.chunk_ok:
    mov [.chunk], eax

    mov ecx, eax
    mov eax, [.cur_lba]
    call read_sectors
    jc .fail

    mov eax, [.chunk]
    mov cl, [BI + BI_SHIFT]
    shl eax, cl
    mov ecx, eax
    mov esi, BOUNCE_ADDR
    mov edi, [.dest]
    call copy_to_high
    add [.dest], eax

    mov eax, [.chunk]`
    add [.cur_lba], eax
    sub [.remaining], eax
    mov cl, [.subshift]
    shl eax, cl
    add [.loaded], eax

    mov al, '.'
    call putc
    jmp .loop

.done:
    popad
    mov ecx, [.loaded]
    clc
    ret
.fail:
    popad
    mov ecx, [.loaded]
    stc
    ret

.dest:      dd 0
.cur_lba:   dd 0
.remaining: dd 0
.chunk:     dd 0
.loaded:    dd 0
.subshift:  db 0

; =============================================================================
;  read_sectors
; =============================================================================
read_sectors:
    pushad
    test byte [BI + BI_FLAGS], BOOTF_EDD
    jz .chs

    mov [dap.lba], eax
    mov [dap.count], cx
    mov word [dap.off], 0
    mov word [dap.seg], BOUNCE_SEG
    mov bp, 3
.lba_retry:
    mov si, dap
    mov ah, 0x42
    mov dl, [BI + BI_DRIVE]
    int 0x13
    jnc .ok
    call disk_reset
    dec bp
    jnz .lba_retry
    jmp .fail

.chs:
    mov [.lba], eax
    mov [.left], cx
    mov word [.off], 0
.chs_loop:
    cmp word [.left], 0
    je .ok
    mov eax, [.lba]
    xor edx, edx
    movzx ecx, word [geo_spt]
    div ecx
    inc dl
    mov [.sector], dl
    xor edx, edx
    movzx ecx, word [geo_heads]
    div ecx
    mov [.head], dl
    mov [.cyl], ax
    mov ax, [geo_spt]
    sub al, [.sector]
    inc ax
    cmp ax, [.left]
    jbe .n_ok
    mov ax, [.left]
.n_ok:
    mov [.n], al
    mov bp, 3
.chs_retry:
    mov ax, [.cyl]
    mov ch, al
    mov cl, ah
    shl cl, 6
    or cl, [.sector]
    mov dh, [.head]
    mov dl, [BI + BI_DRIVE]
    mov ax, BOUNCE_SEG
    mov es, ax
    mov bx, [.off]
    mov ah, 0x02
    mov al, [.n]
    int 0x13
    jnc .chs_next
    call disk_reset
    dec bp
    jnz .chs_retry
    xor ax, ax
    mov es, ax
    jmp .fail
.chs_next:
    xor ax, ax
    mov es, ax
    movzx eax, byte [.n]
    sub [.left], ax
    add [.lba], eax
    shl ax, 9
    add [.off], ax
    jmp .chs_loop

.ok:
    popad
    clc
    ret
.fail:
    popad
    stc
    ret

.lba:    dd 0
.left:   dw 0
.off:    dw 0
.cyl:    dw 0
.head:   db 0
.sector: db 0
.n:      db 0

disk_reset:
    pusha
    xor ah, ah
    mov dl, [BI + BI_DRIVE]
    int 0x13
    popa
    ret

dap:
    db 0x10, 0
.count: dw 0
.off:   dw 0
.seg:   dw 0
.lba:   dq 0

; =============================================================================
get_geometry:
    pusha
    push es
    mov ah, 0x08
    mov dl, [BI + BI_DRIVE]
    xor di, di
    mov es, di
    int 0x13
    jc .default
    and cl, 0x3F
    jz .default
    movzx ax, cl
    mov [geo_spt], ax
    movzx ax, dh
    inc ax
    mov [geo_heads], ax
    jmp .done
.default:
    mov word [geo_spt], 18
    mov word [geo_heads], 2
.done:
    pop es
    popa
    ret

geo_spt:   dw 18
geo_heads: dw 2

; =============================================================================
detect_memory:
    pushad
    mov ax, 0xE801
    xor cx, cx
    xor dx, dx
    int 0x15
    jc .done
    test ax, ax
    jnz .have_ax
    mov ax, cx
    mov bx, dx
.have_ax:
    movzx eax, ax
    movzx ebx, bx
    shl ebx, 6
    add eax, ebx
    add eax, 1024
    mov [BI + BI_MEM_TOTAL_KB], eax
.done:
    popad
    ret

; =============================================================================
;  A20
; =============================================================================
test_a20:
    push ds
    push es
    push si
    push di
    push bx
    pushf
    cli
    xor ax, ax
    mov ds, ax
    mov ax, 0xFFFF
    mov es, ax
    mov si, 0x0500
    mov di, 0x0510
    mov bl, [ds:si]
    mov bh, [es:di]
    mov byte [ds:si], 0x00
    mov byte [es:di], 0xFF
    mov al, [ds:si]
    mov [es:di], bh
    mov [ds:si], bl
    popf
    cmp al, 0xFF
    pop bx
    pop di
    pop si
    pop es
    pop ds
    ret

enable_a20:
    push ax
    call test_a20
    jnz .ok
    mov ax, 0x2401
    int 0x15
    call test_a20
    jnz .ok
    in al, 0x92
    test al, 2
    jnz .skip_fast
    or al, 2
    and al, 0xFE
    out 0x92, al
.skip_fast:
    call test_a20
    jnz .ok
    call kbc_enable_a20
    call test_a20
    jnz .ok
    pop ax
    stc
    ret
.ok:
    pop ax
    clc
    ret

kbc_enable_a20:
    pusha
    call kbc_wait_input
    mov al, 0xAD
    out 0x64, al
    call kbc_wait_input
    mov al, 0xD0
    out 0x64, al
    call kbc_wait_output
    in al, 0x60
    push ax
    call kbc_wait_input
    mov al, 0xD1
    out 0x64, al
    call kbc_wait_input
    pop ax
    or al, 2
    out 0x60, al
    call kbc_wait_input
    mov al, 0xAE
    out 0x64, al
    call kbc_wait_input
    popa
    ret

kbc_wait_input:
    push cx
    mov cx, KBC_TIMEOUT
.l: in al, 0x64
    test al, 2
    loopnz .l
    pop cx
    ret

kbc_wait_output:
    push cx
    mov cx, KBC_TIMEOUT
.l: in al, 0x64
    test al, 1
    loopz .l
    pop cx
    ret

; =============================================================================
;  Unreal mode / защищённый режим
; =============================================================================
unreal_mode:
    push eax
    push bx
    lgdt [gdt_descriptor]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short $+2
    mov bx, GDT_DATA
    mov ds, bx
    mov es, bx
    and al, 0xFE
    mov cr0, eax
    jmp short $+2
    xor bx, bx
    mov ds, bx
    mov es, bx
    pop bx
    pop eax
    ret

copy_to_high:
    push ecx
    push esi
    push edi
    pushf
    cli
    call unreal_mode
    shr ecx, 2
    a32 rep movsd
    popf
    pop edi
    pop esi
    pop ecx
    ret

enter_protected_mode:
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp GDT_CODE:pm_entry

[bits 32]
pm_entry:
    mov ax, GDT_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, PM_STACK_TOP
    mov ebx, BOOTINFO_ADDR
    mov eax, BOOTINFO_MAGIC
    mov ecx, [BOOTINFO_ADDR + BI_KERNEL_ADDR]
    jmp ecx

[bits 16]

align 8
gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt_data:
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start
GDT_CODE equ gdt_code - gdt_start
GDT_DATA equ gdt_data - gdt_start

; =============================================================================
;  Вывод через BIOS
; =============================================================================
putc:
    pusha
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    popa
    ret

puts:
    pusha
.l: lodsb
    test al, al
    jz .d
    call putc
    jmp .l
.d: popa
    ret

newline:
    pusha
    mov al, 13
    call putc
    mov al, 10
    call putc
    popa
    ret

print_hex32:
    pushad
    mov ecx, 8
.l: rol eax, 4
    push eax
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .p
    add al, 7
.p: call putc
    pop eax
    loop .l
    popad
    ret

print_hex8:
    pushad
    shl eax, 24
    mov ecx, 2
.l: rol eax, 4
    push eax
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .p
    add al, 7
.p: call putc
    pop eax
    loop .l
    popad
    ret

vga_disable_blink:
    pusha
    mov dx, 0x3DA
    in al, dx
    mov dx, 0x3C0
    mov al, 0x30
    out dx, al
    inc dx
    in al, dx
    and al, 0xF7
    dec dx
    out dx, al
    popa
    ret

; =============================================================================
;  Данные / сообщения
; =============================================================================
msg_banner:     db 13, 10, "PhLo", 13, 10, 0
msg_drive:      db "boot drive=0x", 0
msg_hdd:        db " HDD/USB", 0
msg_floppy:     db " floppy, CHS", 0
msg_cdrom:      db " CD-ROM, El Torito", 0
msg_iso:        db " ISO", 0
msg_image_lba:  db ", image LBA=0x", 0
msg_kernel_lba: db "loading kernel (LBA) ", 0
msg_ramdisk:    db "copying image to RAM ", 0
msg_ok:         db " OK", 13, 10, 0
msg_partial:    db " PARTIAL (read errors, filesystem may be incomplete)", 13, 10, 0
msg_skipped:    db " skipped", 13, 10, 0
msg_jump:       db "starting kernel", 13, 10, 0
msg_err:        db 13, 10, "BOOT ERROR: ", 0
msg_err_header: db "bad image header", 0
msg_err_a20:    db "cannot enable A20", 0
msg_err_kernel: db "kernel read failed", 0

msg_fs_try:                 db "checking /sys/kernel in filesystem...", 13, 10, 0
msg_fs_kernel_fail:         db "FS kernel not found, fallback to LBA", 13, 10, 0
msg_fs_kernel_ok:           db "FS kernel loaded OK", 13, 10, 0
msg_loading_fs_kernel:      db "loading FS kernel: ", 0
msg_newline:                db 13, 10, 0
msg_kernel_list_header:     db 13, 10, "available kernels in /sys/kernel:", 13, 10, 0
msg_kernel_list_item:       db "  ", 0
msg_default_tag:            db " [default]", 0
msg_kernel_select_prompt:   db 13, 10, "select kernel [0-9] or enter for default (", 0
msg_prompt_end:             db "): ", 0

name_sys:           db "sys",0
name_kernel:        db "kernel",0
name_kernel_bin:    db "kernel.bin",0
name_bootcfg:       db "boot.cfg",0
name_default:       db "default",0
name_active:        db "active",0
name_versions:      db "versions.txt",0
name_readme:        db "README.txt",0

fs_lba_512:         dd 0
fs_total_nodes:     dd 0
fs_nodes_sectors:   dd 0
fs_root_id:         dd 0
fs_sys_id:          dd 0
fs_kernel_dir_id:   dd 0
fs_kernel_file_id:  dd 0
fs_kernel_size:     dd 0
fs_kernel_data_block: dd 0
fs_bootcfg_id:      dd 0
fs_kernel_count:    dd 0
fs_default_kernel_idx: dd 0