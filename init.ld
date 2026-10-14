ENTRY(init)
SECTIONS
{
    /* Основная ОС ложится в 1 МБ */
    . = 0x100000;

    .text : {
        *(.text.main)
        *(.text)
    }
    .data : {
        *(.data)
    }
    /* .bss в init.bin не попадает (файл заканчивается на .data), поэтому
     * init() обнуляет её сама по этим символам (kernel/kernel.c) */
    .bss : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
}
