ENTRY(main)

SECTIONS
{
    . = 0x9000;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(0x10) : {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(0x10) : {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(0x10) : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }
}
