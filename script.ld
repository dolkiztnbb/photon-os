ENTRY(start)

SECTIONS
{

        . = 0x9000;
    
    .text : {
        *(.text)
    }

    .rodata ALIGN(0x800) : {
        *(.rodata)
    }

    .data ALIGN(0x100) : {
        *(.data)
    }

    .bss ALIGN(0x100) : {
        *(COMMON)
        *(.bss)
    }
}
