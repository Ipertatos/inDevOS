OUTPUT_FORMAT(elf64-x86-64)
ENTRY(start)
SECTIONS
{
	. = 0x00110000;
	.boot :
	{
		. = ALIGN(8);
		KEEP(*(.multiboot))
		*(.mbhdr)
		_loadStart = .;
		*(.boot)
		. = ALIGN(4096);
		Pml4 = .;
		. += 0x1000;
		Pdpt = .;
		. += 0x1000;
		Pd = .;
		. += 0x1000;
		. += 0x8000;
		Stack = .;
	}
	.text	: { *(.text .text.*) }
	.data	: { *(.data .data.*) }
	.rodata : { *(.rodata .rodata.*) }
	.bss	: { *(.bss .bss.*) }
}