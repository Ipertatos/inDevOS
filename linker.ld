OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
	text	PT_LOAD		FLAGS((1 << 0) | (1 << 2)) ;	/*EXEC + READ	*/
	rodata	PT_LOAD		FLAGS((1 << 2)) ; 				/*READ ONLY		*/
	data	PT_LOAD		FLAGS((1 << 1) | (1 << 2)) ;	/*WRITE + READ	*/
	dynamic	PT_DYNAMIC	FLAGS((1 << 1) | (1 << 2)) ;	/* DYNAMIC PHDR */
}
SECTIONS
{
	. = 0xffffffff80000000;

	.text : {
		*(.text .text.*)
	} :text

	. += CONSTANT(MAXPAGESIZE);

	.rodata : {
        *(.rodata .rodata.*)
    } :rodata

	. += CONSTANT(MAXPAGESIZE);
 
    .data : {
        *(.data .data.*)
    } :data

    .dynamic : {
        *(.dynamic)
    } :data :dynamic

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
	}
}