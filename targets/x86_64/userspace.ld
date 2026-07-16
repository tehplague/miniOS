OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Userspace PIE linker script — no fixed base; kernel loads ET_DYN at ELF_PIE_BASE */
/* . = SIZEOF_HEADERS causes lld to start sections right after the ELF header+phdrs,
 * keeping them all in the same PT_LOAD (file offset 0).  This is required for
 * MLIBC_STATIC_BUILD which reads the ELF header via __ehdr_start at runtime. */
SECTIONS
{
    . = SIZEOF_HEADERS;

    .text . : {
        __text_start = .;
        *(.text)
    }
    
    .rodata : {
        *(.rodata*)
    }
    
    .data : {
        *(.data)
    }

    .tdata : {
        __tdata_start = .;
        *(.tdata .tdata.*)
        __tdata_end = .;
    }

    .tbss : {
        __tbss_start = .;
        *(.tbss .tbss.*)
        __tbss_end = .;
    }

    .preinit_array : {
        __preinit_array_start = .;
        KEEP(*(.preinit_array))
        __preinit_array_end = .;
    }

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    __bss_end = .;

    /* Define end symbol for heap — placeholder; brk is set by kernel from image_end */
    end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note*)
        *(.comment)
    }
}
