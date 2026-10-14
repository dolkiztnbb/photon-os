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
    .bss : {
        *(.bss)
        *(COMMON)
    }
}