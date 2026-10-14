ENTRY(kernel_main)

SECTIONS
{
    . = 0x9000;

    .text : {
	    *(.text.kernel_main) /* Если вы добавите атрибут секции в Си */
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
