OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)

SECTIONS
{
    . = 0x00000000;

    .header : ALIGN(4)
    {
        KEEP(*(.app_header))
    }

    .preamble : ALIGN(4)
    {
        KEEP(*(.preamble))
    }

    .text : ALIGN(4)
    {
        *(.text*)
    }

    .rodata : ALIGN(4)
    {
        *(.rodata*)
    }

    . = ALIGN(4);
    __code_size__ = . ;

    .data : ALIGN(4)
    {
        *(.data*)
    }

    . = ALIGN(4);
    __data_end__ = . ;
    __data_size__ = __data_end__ - __code_size__ ;

    .bss : ALIGN(4)
    {
        *(.bss*)
        *(COMMON)
    }

    . = ALIGN(4);
    __bss_end__ = . ;
    __bss_size__ = __bss_end__ - __data_end__ ;
}
