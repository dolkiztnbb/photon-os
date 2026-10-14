; Identical to lesson 13's boot sector, but the %included files have new paths
[org 0x7c00]
KERNEL_OFFSET equ 0x9000 ; The same one we used when linking the kernel
    ;pusha
    ;mov ah, 00h
    ;mov al, 13h
    ;int 10h
    ;popa
    pusha
    mov dx, 3DAh       ; Address of Input Status #1 register
    in al, dx          ; Reset flip-flop to index mode
    mov dx, 3C0h       ; Address of Attribute Controller
    mov al, 10h        ; Index 10h: Mode Control Register
    out dx, al
    in al, dx          ; Read current value
    and al, 11110111b  ; Clear bit 3 (Disable Blink, Enable Intensity)
    out dx, al         ; Write back
    popa
    pusha
    ; Disable blinking on 8th bit set to 1 
	mov dx,0x3da
	in al,dx

	mov dx,0x3c0
	mov al,30h
	out dx,al

	inc dx
	in al,dx
	and al,8
	dec dx
	out dx,al
    popa

    mov [BOOT_DRIVE], dl ; Remember that the BIOS sets us the boot drive in 'dl' on boot
    mov bp, 0x8000
    mov sp, bp

	pusha
        mov dx,[BOOT_DRIVE]   ; we do this so we can read from 32 bit mode the drive we've booted from
	    mov [0x100], dx     ; Actually i think it does...
        call print_hex
	popa

    mov bx, MSG_REAL_MODE 
    call print
    call print_nl

    call load_kernel ; read the kernel from disk
    call switch_to_pm ; disable interrupts, load GDT,  etc. Finally jumps to 'BEGIN_PM'
    nop ; mov al, 0xe
    nop ; out 0xcf9,al ; jmp 0x0 ; Never executed
    nop
    jmp 0x7c00
%include "boot/print.asm"
%include "boot/print_hex.asm"
%include "boot/disk.asm"
%include "boot/32bit_print.asm"
%include "boot/switch_pm.asm"

[bits 16]
load_kernel:
    mov bx, MSG_LOAD_KERNEL
    call print
    call print_nl
    push ax
    mov ax, 0
    mov es, ax
    pop ax
    mov bx, KERNEL_OFFSET ; Read from disk and store in 0x1000
    mov dh, 63; Our future kernel will be larger, make this big
    mov dl, [BOOT_DRIVE]
    call disk_load
    ;call disk_load

    ret

[bits 32]
BEGIN_PM:
    mov ebx, MSG_PROT_MODE
    call print_string_pm
    call KERNEL_OFFSET ; Give control to the kernel
    mov ebx, MSG_RETURNED_KERNEL
    call print_string_pm
    jmp $


BOOT_DRIVE db 0x69 ; It is a good idea to store it in memory because 'dl' may get overwritten
MSG_REAL_MODE db ";SRM;", 0 ; in real mode
MSG_PROT_MODE db "LPM;", 0 ; in protected mode
MSG_LOAD_KERNEL db "LK;", 0 ; loading kernel
MSG_RETURNED_KERNEL db "R;", 0 ; return from kernel

%include "boot/gdt.asm"
times 508 - ($-$$) db 0
dw 0x3003
dw 0xaa55
