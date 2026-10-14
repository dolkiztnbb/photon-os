; =============================================================================
;  PhotonOS — stage 2 загрузчика
;
;  Запускается из bootsect.asm по адресу STAGE2_ADDR (0x8200) в реальном
;  режиме: DS=ES=SS=0, SP=0x7C00, DL = загрузочный диск, bootinfo (0x600)
;  частично заполнен (magic, drive, sector_shift, image_lba, flags).
;  Копия первых 16 КБ образа (MBR + stage2) лежит по IMAGE_HEAD_ADDR.
;
;  Что делаем:
;    1. определяем тип носителя (HDD/floppy/CD), геометрию для CHS;
;    2. включаем A20, определяем объём памяти (int 15h E801);
;    3. включаем «unreal mode» (DS/ES с лимитом 4 ГБ) — это позволяет
;       класть данные выше 1 МБ, продолжая пользоваться BIOS;
;    4. ПЫТАЕМСЯ загрузить ядро из ФС /sys/kernel (поддержка версий);
;       если не получилось — fallback на старый метод (init.bin по LBA);
;    5. копируем ВЕСЬ образ в ОЗУ по RAMDISK_ADDR — ядро использует эту
;       копию как блочное устройство, если у него нет собственного драйвера
;       для загрузочного носителя (USB, SATA CD-ROM и т.п.);
;    6. заполняем bootinfo и прыгаем в 32-битный защищённый режим на ядро.
;
;  Все параметры раскладки образа берутся из заголовка pht_imghdr_t в MBR,
;  поэтому stage2 собирается без -D флагов.
; =============================================================================

%include "boot/bootinfo.inc"

BI              equ BOOTINFO_ADDR
HDR             equ IMAGE_HEAD_ADDR + IMGHDR_OFFSET
KBC_TIMEOUT     equ 0xFFFF

; --- дополнительные адреса для FS-загрузчика ---------------------------------
; Располагаем ниже 64КБ, чтобы доступ через SI работал в реальном режиме
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

    ; копия первого сектора образа (как его видит BIOS) — по ней ядро находит
    ; загрузочный диск среди устройств своих драйверов
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
    add eax, ecx                    ; eax = конец ramdisk
    mov edx, [BI + BI_MEM_TOTAL_KB]
    test edx, edx
    jz .ramdisk_mem_ok              ; память неизвестна — рискуем
    shl edx, 10
    cmp eax, edx
    ja .ramdisk_skip
.ramdisk_mem_ok:
    xor eax, eax                    ; с самого начала образа
    mov ecx, [HDR + IH_IMAGE_SECTORS]
    mov edi, [HDR + IH_RAMDISK_ADDR]
    call load_image_region          ; ошибка не фатальна: ecx = сколько успели
    test ecx, ecx
    jz .ramdisk_skip
    mov [BI + BI_RAMDISK_SECTORS], ecx
    mov eax, [HDR + IH_RAMDISK_ADDR]
    mov [BI + BI_RAMDISK_ADDR], eax
    or byte [BI + BI_FLAGS], BOOTF_RAMDISK
    mov si, msg_ok
    cmp ecx, [HDR + IH_IMAGE_SECTORS]
    jae .ramdisk_msg
    mov si, msg_partial             ; часть секторов не прочиталась
.ramdisk_msg:
    call puts
    jmp .ramdisk_done
.ramdisk_skip:
    mov si, msg_skipped
    call puts
.ramdisk_done:

    ; --- поехали -----------------------------------------------------------------
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
;  fatal: печатаем сообщение из SI и останавливаемся
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
;  FS-загрузчик: попытка загрузить ядро из /sys/kernel
;  Возвращает CF=0 успех, CF=1 неудача
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

    mov eax, [BOUNCE_ADDR]
    cmp eax, FS_MAGIC
    jne .fail

    mov eax, [BOUNCE_ADDR+4] ; total_nodes
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
    inc eax ; FS_LBA+1
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

    ; --- пробуем прочитать boot.cfg / default для выбора версии ---------------
    mov eax, [fs_kernel_dir_id]
    mov si, name_bootcfg
    call fs_find_child
    jnc .have_bootcfg
    mov eax, [fs_kernel_dir_id]
    mov si, name_default
    call fs_find_child
    jnc .have_bootcfg
    mov eax, [fs_kernel_dir_id]
    mov si, name_active
    call fs_find_child
    jnc .have_bootcfg
    jmp .no_bootcfg

.have_bootcfg:
    mov [fs_bootcfg_id], eax
    ; загружаем boot.cfg в FS_BOOTCFG_BUF
    mov eax, [fs_bootcfg_id]
    mov edi, FS_BOOTCFG_BUF
    call fs_load_file_by_id
    jc .no_bootcfg
    ; парсим имя файла из FS_BOOTCFG_BUF в FS_TMP_NAME_ADDR
    mov si, FS_BOOTCFG_BUF
    mov di, FS_TMP_NAME_ADDR
    call fs_parse_filename
    ; ищем файл с таким именем в /sys/kernel
    mov eax, [fs_kernel_dir_id]
    mov si, FS_TMP_NAME_ADDR
    call fs_find_child
    jnc .found_kernel

.no_bootcfg:
    ; --- пробуем kernel.bin ----------------------------------------------------
    mov eax, [fs_kernel_dir_id]
    mov si, name_kernel_bin
    call fs_find_child
    jnc .found_kernel

    ; --- ищем любой .bin или первый файл в /sys/kernel -------------------------
    call fs_list_kernels_and_select
    jc .fail
    jmp .found_kernel_id_in_eax

.found_kernel:
    mov [fs_kernel_file_id], eax
    jmp .load_kernel_file

.found_kernel_id_in_eax:
    mov [fs_kernel_file_id], eax

.load_kernel_file:
    ; загружаем выбранный файл ядра
    mov ebx, FS_NODES_ADDR
    mov ecx, [fs_kernel_file_id]
    shl ecx, 6
    add ebx, ecx ; ebx = node ptr
    mov eax, [ebx + NODE_SIZE_FIELD_OFF]
    mov edx, [ebx + NODE_DATA_BLOCK_OFF]
    mov [fs_kernel_size], eax
    mov [fs_kernel_data_block], edx
    test eax, eax
    jz .fail

    mov si, msg_loading_fs_kernel
    call puts
    push si
    mov si, ebx
    add si, NODE_NAME_OFF
    call puts
    pop si
    mov si, msg_newline
    call puts

    mov eax, [fs_lba_512]
    add eax, edx ; FS_LBA + data_block
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
; fs_find_root: найти узел с parent_id == -1
; выход: EAX = id корня, CF=0 успех
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
; fs_find_child: найти дочерний узел по parent_id и имени
; вход: EAX = parent_id, SI = имя (ASCIIZ)
; выход: EAX = node id, EBX = node ptr, CF=0 успех
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
    add ebx, eax ; ebx = node ptr
    mov eax, [ebx + NODE_PARENT_OFF]
    cmp eax, [fs_find_parent]
    jne .next
    ; сравниваем имя
    mov si, [fs_find_name_ptr]
    mov di, ebx
    add di, NODE_NAME_OFF
    call fs_streq
    jnc .found
.next:
    inc edx
    jmp .loop
.found:
    mov eax, edx
    ; ebx уже указывает на узел
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

fs_find_parent: dd 0
fs_find_name_ptr: dw 0,0 ; actually 32-bit pointer but we use SI offset in low mem, so store as dd

; -----------------------------------------------------------------------------
; fs_streq: сравнить строку SI (ASCIIZ) с именем DI (32 байта, null-term)
; выход: CF=0 равны, CF=1 не равны
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
    ; если дошли до 32 символов и SI тоже закончился (или следующий 0) — равны
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
; fs_load_file_by_id: загрузить файл по id в EDI
; вход: EAX = node id, EDI = phys dest
; выход: CF
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
    ; EDI уже dest
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
; fs_parse_filename: извлечь первое слово из файла (до \r \n \0 space)
; вход: SI = source buffer, DI = dest buffer (FS_TMP_NAME_ADDR)
; выход: DI указывает на dest (null-term)
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
    ; первый символ имени
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
; fs_list_kernels_and_select: перечислить файлы в /sys/kernel и дать выбор
; вход: fs_kernel_dir_id установлен
; выход: EAX = выбранный node id, CF=0 успех
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
    xor ebx, ebx ; count
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
    ; опционально фильтруем .bin, но берём все файлы кроме boot.cfg/default/active
    mov si, ebx
    add si, NODE_NAME_OFF
    push si
    mov si, name_bootcfg
    mov di, ebx
    add di, NODE_NAME_OFF
    call fs_streq
    pop si
    jnc .collect_next
    mov si, name_default
    mov di, ebx
    add di, NODE_NAME_OFF
    push ebx
    call fs_streq
    pop ebx
    jnc .collect_next
    mov si, name_active
    mov di, ebx
    add di, NODE_NAME_OFF
    push ebx
    call fs_streq
    pop ebx
    jnc .collect_next

    ; это кандидат
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

    cmp ebx, 1
    je .single

    ; --- показать список -----------------------------------------------------
    mov si, msg_kernel_list_header
    call puts
    xor edx, edx
.print_loop:
    cmp edx, ebx
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
    ; имя файла
    mov esi, FS_LIST_ADDR
    mov eax, edx
    shl eax, 2
    add esi, eax
    mov eax, [esi] ; id
    mov esi, FS_NODES_ADDR
    shl eax, 6
    add esi, eax
    add esi, NODE_NAME_OFF
    mov si, si ; si already low 16? need full? SI is 16-bit, but ESI may be >64K? FS_NODES_ADDR 0x18000 <64K, so SI ok
    ; Actually we have 32-bit ESI, but puts expects SI (16-bit) pointing to low mem string.
    ; Since nodes are in low mem (<0x20000), we can use SI = low 16 bits
    call puts
    call newline
    inc edx
    jmp .print_loop

.ask:
    mov si, msg_kernel_select_prompt
    call puts

.wait_key:
    mov ah, 0
    int 0x16 ; AL=ASCII
    cmp al, 13 ; Enter
    je .select_default
    cmp al, '0'
    jb .wait_key
    cmp al, '9'
    ja .wait_key
    sub al, '0'
    movzx edx, al
    cmp edx, [fs_kernel_count]
    jae .wait_key
    ; echo
    call putc
    call newline
    jmp .selected

.select_default:
    mov si, msg_newline
    call puts
    xor edx, edx

.selected:
    mov esi, FS_LIST_ADDR
    mov eax, edx
    shl eax, 2
    add esi, eax
    mov eax, [esi]
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    clc
    ret

.single:
    mov esi, FS_LIST_ADDR
    mov eax, [esi]
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
;  load_image_region — загрузить кусок образа в физическую память
;    EAX = первый 512-байтный сектор ВНУТРИ образа
;    ECX = количество 512-байтных секторов
;    EDI = физический адрес назначения (любой, в т.ч. > 1 МБ)
;  Выход: CF=0 — всё загружено; CF=1 — ошибка. ECX = сколько 512-байтных
;  секторов реально скопировано (при ошибке — кратно размеру куска).
; =============================================================================
load_image_region:
    pushad
    mov [.dest], edi
    mov [.remaining], ecx           ; пока в 512-байтных секторах
    mov dword [.loaded], 0

    ; сектора(512) -> сектора BIOS: сдвиг = BI_SHIFT - 9 (0 для HDD, 2 для CD)
    mov cl, [BI + BI_SHIFT]
    sub cl, 9
    mov [.subshift], cl
    shr eax, cl
    add eax, [BI + BI_IMAGE_LBA]
    mov [.cur_lba], eax             ; текущий LBA на носителе
    mov eax, [.remaining]
    mov edx, 1
    shl edx, cl
    dec edx
    add eax, edx                    ; округляем вверх
    shr eax, cl
    mov [.remaining], eax           ; теперь в секторах BIOS

.loop:
    mov eax, [.remaining]
    test eax, eax
    jz .done

    ; размер куска: не больше BOUNCE_BYTES
    mov ebx, BOUNCE_BYTES
    mov cl, [BI + BI_SHIFT]
    shr ebx, cl                     ; секторов BIOS помещается в буфер
    cmp eax, ebx
    jbe .chunk_ok
    mov eax, ebx
.chunk_ok:
    mov [.chunk], eax

    mov ecx, eax
    mov eax, [.cur_lba]
    call read_sectors               ; -> BOUNCE_SEG:0
    jc .fail

    ; копируем кусок по назначению (выше 1 МБ — через unreal mode)
    mov eax, [.chunk]
    mov cl, [BI + BI_SHIFT]
    shl eax, cl                     ; eax = байт в куске
    mov ecx, eax
    mov esi, BOUNCE_ADDR
    mov edi, [.dest]
    call copy_to_high
    add [.dest], eax

    mov eax, [.chunk]
    add [.cur_lba], eax
    sub [.remaining], eax
    mov cl, [.subshift]
    shl eax, cl                     ; 512-байтных секторов в куске
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
;  read_sectors — прочитать ECX секторов BIOS начиная с LBA EAX в BOUNCE_SEG:0
;  Выход: CF=1 при ошибке. Использует EDD (AH=42h) либо CHS (AH=02h).
; =============================================================================
read_sectors:
    pushad
    test byte [BI + BI_FLAGS], BOOTF_EDD
    jz .chs

    ; --- LBA -------------------------------------------------------------------
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

    ; --- CHS: читаем дорожку за дорожкой, не пересекая границу дорожки ---------
.chs:
    mov [.lba], eax
    mov [.left], cx
    mov word [.off], 0
.chs_loop:
    cmp word [.left], 0
    je .ok
    ; lba -> C/H/S
    mov eax, [.lba]
    xor edx, edx
    movzx ecx, word [geo_spt]
    div ecx                         ; eax = дорожка, edx = сектор-1
    inc dl
    mov [.sector], dl
    xor edx, edx
    movzx ecx, word [geo_heads]
    div ecx                         ; eax = цилиндр, edx = головка
    mov [.head], dl
    mov [.cyl], ax
    ; сколько секторов осталось до конца дорожки
    mov ax, [geo_spt]
    sub al, [.sector]
    inc ax                          ; spt - sector + 1
    cmp ax, [.left]
    jbe .n_ok
    mov ax, [.left]
.n_ok:
    mov [.n], al
    mov bp, 3
.chs_retry:
    mov ax, [.cyl]
    mov ch, al                      ; младшие 8 бит цилиндра
    mov cl, ah
    shl cl, 6                       ; биты 8-9 цилиндра -> биты 6-7 CL
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
;  get_geometry — геометрия для CHS (int 13h AH=08h), по умолчанию 18/2
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
;  detect_memory — int 15h AX=E801h -> [BI_MEM_TOTAL_KB] (0 если не удалось)
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
    mov ax, cx                      ; некоторые BIOS отвечают только в CX/DX
    mov bx, dx
.have_ax:
    movzx eax, ax                   ; КБ между 1 и 16 МБ
    movzx ebx, bx                   ; блоки по 64 КБ выше 16 МБ
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
; test_a20: ZF=1 если A20 ВЫКЛЮЧЕНА (адреса заворачиваются), ZF=0 если включена
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
    mov si, 0x0500                  ; 0000:0500 = 0x000500
    mov di, 0x0510                  ; FFFF:0510 = 0x100500
    mov bl, [ds:si]                 ; сохраняем старое содержимое
    mov bh, [es:di]
    mov byte [ds:si], 0x00
    mov byte [es:di], 0xFF
    mov al, [ds:si]
    mov [es:di], bh                 ; восстанавливаем (mov флаги не трогает)
    mov [ds:si], bl
    popf
    cmp al, 0xFF                    ; совпало -> заворот -> A20 выключена (ZF=1)
    pop bx
    pop di
    pop si
    pop es
    pop ds
    ret

; enable_a20: CF=0 — включена, CF=1 — не удалось
enable_a20:
    push ax
    call test_a20
    jnz .ok
    ; 1) BIOS
    mov ax, 0x2401
    int 0x15
    call test_a20
    jnz .ok
    ; 2) Fast A20 (порт 0x92)
    in al, 0x92
    test al, 2
    jnz .skip_fast
    or al, 2
    and al, 0xFE                    ; бит 0 — reset, не трогаем!
    out 0x92, al
.skip_fast:
    call test_a20
    jnz .ok
    ; 3) контроллер клавиатуры
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
    mov al, 0xAD                    ; отключить клавиатуру
    out 0x64, al
    call kbc_wait_input
    mov al, 0xD0                    ; прочитать выходной порт
    out 0x64, al
    call kbc_wait_output
    in al, 0x60
    push ax
    call kbc_wait_input
    mov al, 0xD1                    ; записать выходной порт
    out 0x64, al
    call kbc_wait_input
    pop ax
    or al, 2
    out 0x60, al
    call kbc_wait_input
    mov al, 0xAE                    ; включить клавиатуру
    out 0x64, al
    call kbc_wait_input
    popa
    ret

kbc_wait_input:                     ; ждём, пока входной буфер опустеет
    push cx
    mov cx, KBC_TIMEOUT
.l: in al, 0x64
    test al, 2
    loopnz .l
    pop cx
    ret

kbc_wait_output:                    ; ждём данные в выходном буфере
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
; unreal_mode: загрузить в DS и ES дескрипторы с лимитом 4 ГБ и вернуться в
; реальный режим. После этого 32-битная адресация [esi]/[edi] работает для
; всего адресного пространства. BIOS может сбросить лимиты, поэтому вызываем
; перед каждым копированием (с запрещёнными прерываниями).
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
    mov ds, bx                      ; база 0, лимит остаётся 4 ГБ
    mov es, bx
    pop bx
    pop eax
    ret

; copy_to_high: ECX байт из ESI (ниже 1 МБ) в EDI (любой физический адрес)
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
    mov ebx, BOOTINFO_ADDR          ; указатель на bootinfo для ядра
    mov eax, BOOTINFO_MAGIC
    mov ecx, [BOOTINFO_ADDR + BI_KERNEL_ADDR]
    jmp ecx

[bits 16]

; --- GDT: null, code32, data32 (плоская модель) -------------------------------
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
putc:                               ; AL = символ
    pusha
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    popa
    ret

puts:                               ; SI -> ASCIIZ
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

print_hex32:                        ; EAX
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

print_hex8:                         ; AL
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

; выключаем мигание (бит 3 атрибута = яркий фон, а не blink) — как раньше в MBR
vga_disable_blink:
    pusha
    mov dx, 0x3DA
    in al, dx                       ; сброс флип-флопа контроллера атрибутов
    mov dx, 0x3C0
    mov al, 0x30                    ; индекс 0x10 | PAS
    out dx, al
    inc dx
    in al, dx
    and al, 0xF7                    ; бит 3 = 0 -> blink off
    dec dx
    out dx, al
    popa
    ret

; =============================================================================
;  Данные / сообщения
; =============================================================================
msg_banner:     db 13, 10, "PhotonOS loader (stage2) v2 - FS kernel support", 13, 10, 0
msg_drive:      db "boot drive: 0x", 0
msg_hdd:        db " [HDD/USB, LBA]", 0
msg_floppy:     db " [floppy, CHS]", 0
msg_cdrom:      db " [CD-ROM, El Torito]", 0
msg_iso:        db " [ISO]", 0
msg_image_lba:  db ", image LBA 0x", 0
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

; --- FS loader messages -------------------------------------------------------
msg_fs_try:                 db "checking /sys/kernel in filesystem...", 13, 10, 0
msg_fs_kernel_fail:         db "FS kernel not found, fallback to LBA", 13, 10, 0
msg_fs_kernel_ok:           db "FS kernel loaded OK", 13, 10, 0
msg_loading_fs_kernel:      db "loading FS kernel: ", 0
msg_newline:                db 13, 10, 0
msg_kernel_list_header:     db 13, 10, "Available kernels in /sys/kernel:", 13, 10, 0
msg_kernel_list_item:       db "  ", 0
msg_kernel_select_prompt:   db 13, 10, "Select kernel [0-9] or Enter for default: ", 0

; --- FS names -----------------------------------------------------------------
name_sys:           db "sys",0
name_kernel:        db "kernel",0
name_kernel_bin:    db "kernel.bin",0
name_bootcfg:       db "boot.cfg",0
name_default:       db "default",0
name_active:        db "active",0

; --- FS loader variables ------------------------------------------------------
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
