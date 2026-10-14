; context_switch.asm
global switch_context

switch_context:
    pushad          ; Save all general purpose registers
    
    ; Save current ESP to first parameter
    mov eax, [esp + 36]     ; Address of old_esp (pushad pushed 32 bytes + 4 for return addr)
    mov [eax], esp          ; Store current ESP
    add dword [eax], 36     ; Point to saved registers location

    ; Load new ESP from second parameter  
    mov eax, [esp + 40]     ; Address of new_esp
	; emulator crashes here vvvvvvvvvvvvvvvv
    mov esp, [eax]          ; Load new ESP
    
    popad           ; Restore all general purpose registers
    ret
