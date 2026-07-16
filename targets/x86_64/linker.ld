OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH("i386:x86-64")
ENTRY(start)

KERNEL_VMA = 0xFFFF800000000000;

SECTIONS
{
    . = KERNEL_VMA + 0x100000;

    __image_start = .;

    /* Boot section: multiboot header + 32-bit boot code.
       LMA = physical address (VMA - KERNEL_VMA), starting at 1MB. */
    .boot : AT(ADDR(.boot) - KERNEL_VMA)
    {
        KEEP(*(.multiboot_header))
        *(.boot)
    }

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text .text.*)
        *(.ltext .ltext.*)
        *(.rodata .rodata.*)
        *(.lrodata .lrodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data .data.*)
        *(.ldata .ldata.*)
        /* AP startup trampoline blob: copied at runtime to physical 0x8000.
           Placed in .data so trampoline_start/trampoline_end are accessible. */
        *(.trampoline)
    }

    __bss_start = .;
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        *(.bss .bss.*)
        *(.lbss .lbss.*)
    }
    __bss_end = .;
    __image_end = .;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note*)
        *(.comment)
    }
}
