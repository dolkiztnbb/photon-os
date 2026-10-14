; Defined in isr.c
[extern isr_handler]
[extern irq_handler]

; Generic interrupt stub that works for any interrupt vector
; The interrupt number will be passed as a parameter
interrupt_common_stub:
    ; 1. Save CPU state
    pushad                    ; Pushes edi, esi, ebp, esp, ebx, edx, ecx, eax

    ; Save segment registers (16-bit values)
    push ds
    push es
    push fs
    push gs

    ; Set up kernel segments
    mov ax, 0x10              ; kernel data segment descriptor
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    sti

    push esp                  ; Pass pointer to registers (struct) to C handler

    ; Call C handler (same handler processes all interrupts)
    call isr_handler

    ; 3. Restore state
    add esp, 4                ; Clean up the argument pushed before call
    cli
    ; Restore segment registers
    pop gs
    pop fs
    pop es
    pop ds

    popad

    ; Remove the interrupt number and optional error code from stack
    ; This assumes we always push 2 dwords: error code and interrupt number
    add esp, 8
    iret                      ; Return from interrupt

; Macro to generate interrupt stubs dynamically
%macro MAKE_ISR 1
global isr%1
isr%1:
    push dword 0              ; Dummy error code (for consistency)
    push dword %1             ; Interrupt number
    jmp interrupt_common_stub
%endmacro

; Macro for ISRs that push real error codes (like double fault, page fault, etc.)
%macro MAKE_ISR_WITH_ERR 1
global isr%1
isr%1:
    ; No dummy error code - real one already pushed by CPU
    push dword %1             ; Interrupt number
    jmp interrupt_common_stub
%endmacro

; Generate standard ISRs
MAKE_ISR 0
MAKE_ISR 1
MAKE_ISR 2
MAKE_ISR 3
MAKE_ISR 4
MAKE_ISR 5
MAKE_ISR 6
MAKE_ISR 7
MAKE_ISR_WITH_ERR 8    ; Double fault has error code
MAKE_ISR 9
MAKE_ISR_WITH_ERR 10   ; Invalid TSS has error code
MAKE_ISR_WITH_ERR 11   ; Segment not present has error code
MAKE_ISR_WITH_ERR 12   ; Stack fault has error code
MAKE_ISR_WITH_ERR 13   ; General protection fault has error code
MAKE_ISR_WITH_ERR 14   ; Page fault has error code
MAKE_ISR 15
MAKE_ISR 16
MAKE_ISR 17
MAKE_ISR 18
MAKE_ISR 19
MAKE_ISR 20
MAKE_ISR 21
MAKE_ISR 22
MAKE_ISR 23
MAKE_ISR 24
MAKE_ISR 25
MAKE_ISR 26
MAKE_ISR 27
MAKE_ISR 28
MAKE_ISR 29
MAKE_ISR 30
MAKE_ISR 31
MAKE_ISR 48
MAKE_ISR 49
MAKE_ISR 50
MAKE_ISR 51
MAKE_ISR 52
MAKE_ISR 53
MAKE_ISR 54
MAKE_ISR 55
MAKE_ISR 56
MAKE_ISR 57
MAKE_ISR 58
MAKE_ISR 59
MAKE_ISR 60
MAKE_ISR 61
MAKE_ISR 62
MAKE_ISR 63
MAKE_ISR 64
MAKE_ISR 65
MAKE_ISR 66
MAKE_ISR 67
MAKE_ISR 68
MAKE_ISR 69
MAKE_ISR 70
MAKE_ISR 71
MAKE_ISR 72
MAKE_ISR 73
MAKE_ISR 74
MAKE_ISR 75
MAKE_ISR 76
MAKE_ISR 77
MAKE_ISR 78
MAKE_ISR 79
MAKE_ISR 80
MAKE_ISR 81
MAKE_ISR 82
MAKE_ISR 83
MAKE_ISR 84
MAKE_ISR 85
MAKE_ISR 86
MAKE_ISR 87
MAKE_ISR 88
MAKE_ISR 89
MAKE_ISR 90
MAKE_ISR 91
MAKE_ISR 92
MAKE_ISR 93
MAKE_ISR 94
MAKE_ISR 95
MAKE_ISR 96
MAKE_ISR 97
MAKE_ISR 98
MAKE_ISR 99
MAKE_ISR 100
MAKE_ISR 101
MAKE_ISR 102
MAKE_ISR 103
MAKE_ISR 104
MAKE_ISR 105
MAKE_ISR 106
MAKE_ISR 107
MAKE_ISR 108
MAKE_ISR 109
MAKE_ISR 110
MAKE_ISR 111
MAKE_ISR 112
MAKE_ISR 113
MAKE_ISR 114
MAKE_ISR 115
MAKE_ISR 116
MAKE_ISR 117
MAKE_ISR 118
MAKE_ISR 119
MAKE_ISR 120
MAKE_ISR 121
MAKE_ISR 122
MAKE_ISR 123
MAKE_ISR 124
MAKE_ISR 125
MAKE_ISR 126
MAKE_ISR 127
MAKE_ISR 128
MAKE_ISR 129
MAKE_ISR 130
MAKE_ISR 131
MAKE_ISR 132
MAKE_ISR 133
MAKE_ISR 134
MAKE_ISR 135
MAKE_ISR 136
MAKE_ISR 137
MAKE_ISR 138
MAKE_ISR 139
MAKE_ISR 140
MAKE_ISR 141
MAKE_ISR 142
MAKE_ISR 143
MAKE_ISR 144
MAKE_ISR 145
MAKE_ISR 146
MAKE_ISR 147
MAKE_ISR 148
MAKE_ISR 149
MAKE_ISR 150
MAKE_ISR 151
MAKE_ISR 152
MAKE_ISR 153
MAKE_ISR 154
MAKE_ISR 155
MAKE_ISR 156
MAKE_ISR 157
MAKE_ISR 158
MAKE_ISR 159
MAKE_ISR 160
MAKE_ISR 161
MAKE_ISR 162
MAKE_ISR 163
MAKE_ISR 164
MAKE_ISR 165
MAKE_ISR 166
MAKE_ISR 167
MAKE_ISR 168
MAKE_ISR 169
MAKE_ISR 170
MAKE_ISR 171
MAKE_ISR 172
MAKE_ISR 173
MAKE_ISR 174
MAKE_ISR 175
MAKE_ISR 176
MAKE_ISR 177
MAKE_ISR 178
MAKE_ISR 179
MAKE_ISR 180
MAKE_ISR 181
MAKE_ISR 182
MAKE_ISR 183
MAKE_ISR 184
MAKE_ISR 185
MAKE_ISR 186
MAKE_ISR 187
MAKE_ISR 188
MAKE_ISR 189
MAKE_ISR 190
MAKE_ISR 191
MAKE_ISR 192
MAKE_ISR 193
MAKE_ISR 194
MAKE_ISR 195
MAKE_ISR 196
MAKE_ISR 197
MAKE_ISR 198
MAKE_ISR 199
MAKE_ISR 200
MAKE_ISR 201
MAKE_ISR 202
MAKE_ISR 203
MAKE_ISR 204
MAKE_ISR 205
MAKE_ISR 206
MAKE_ISR 207
MAKE_ISR 208
MAKE_ISR 209
MAKE_ISR 210
MAKE_ISR 211
MAKE_ISR 212
MAKE_ISR 213
MAKE_ISR 214
MAKE_ISR 215
MAKE_ISR 216
MAKE_ISR 217
MAKE_ISR 218
MAKE_ISR 219
MAKE_ISR 220
MAKE_ISR 221
MAKE_ISR 222
MAKE_ISR 223
MAKE_ISR 224
MAKE_ISR 225
MAKE_ISR 226
MAKE_ISR 227
MAKE_ISR 228
MAKE_ISR 229
MAKE_ISR 230
MAKE_ISR 231
MAKE_ISR 232
MAKE_ISR 233
MAKE_ISR 234
MAKE_ISR 235
MAKE_ISR 236
MAKE_ISR 237
MAKE_ISR 238
MAKE_ISR 239
MAKE_ISR 240
MAKE_ISR 241
MAKE_ISR 242
MAKE_ISR 243
MAKE_ISR 244
MAKE_ISR 245
MAKE_ISR 246
MAKE_ISR 247
MAKE_ISR 248
MAKE_ISR 249
MAKE_ISR 250
MAKE_ISR 251
MAKE_ISR 252
MAKE_ISR 253
MAKE_ISR 254
MAKE_ISR 255

; User interrupt
global isr0x80
isr0x80:
    push dword 0
    push dword 0x80
    jmp interrupt_common_stub

; IRQ macros
%macro MAKE_IRQ 1
global irq%1
irq%1:
    push byte 0               ; Dummy error code
    push byte (32+%1)         ; IRQ number mapped to IDT entries 32-47
    jmp interrupt_common_stub
%endmacro

; Generate IRQ handlers
MAKE_IRQ 0
MAKE_IRQ 1
MAKE_IRQ 2
MAKE_IRQ 3
MAKE_IRQ 4
MAKE_IRQ 5
MAKE_IRQ 6
MAKE_IRQ 7
MAKE_IRQ 8
MAKE_IRQ 9
MAKE_IRQ 10
MAKE_IRQ 11
MAKE_IRQ 12
MAKE_IRQ 13
MAKE_IRQ 14
MAKE_IRQ 15
